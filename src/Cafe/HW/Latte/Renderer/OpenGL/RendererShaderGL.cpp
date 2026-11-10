#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/RendererShaderGL.h"

#include "Cemu/FileCache/FileCache.h"
#include "Common/FileStream.h"

#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "Cafe/CafeSystem.h"
#include "RevelationsGLPreparation.h"
#include "RevelationsShaderScheduling.h"
#include "Cafe/HW/Latte/Core/RevelationsEarlyPreparation.h"
#include "Cafe/HW/Latte/Core/RevelationsCommandLookahead.h"
#include <deque>

extern std::atomic_int g_compiled_shaders_total;
extern std::atomic_int g_compiled_shaders_async;

bool s_isLoadingShaders{false};

namespace
{
struct EarlyProgram
{
    uint64 base, aux;
    std::string source;
    std::unique_ptr<RendererShaderGL> shader;
    std::chrono::steady_clock::time_point submitted;
};
std::deque<EarlyProgram> s_earlyPrograms;
bool s_creatingEarlyProgram{}; // GPU thread only; no GL work is done by the producer.
unsigned s_earlySubmitted{}, s_earlyUsed{};

class BuildTimer
{
public:
    BuildTimer(const char* stage, uint64 base, uint64 aux)
        : m_stage(stage), m_base(base), m_aux(aux)
    {
        static const bool requested = std::getenv("CEMU_RE_FRAME_TIMING") != nullptr;
        m_enabled = requested && RevelationsShaderScheduling::Matches(CafeSystem::GetForegroundTitleId(),
            CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated());
        if (m_enabled) m_start = std::chrono::steady_clock::now();
    }
    ~BuildTimer()
    {
        if (!m_enabled) return;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start).count();
        if (ms >= 5.0)
            cemuLog_log(LogType::Force, "[RE:GL] stage={} duration_ms={:.3f} base={:016x} aux={:016x} loading={}",
                m_stage, ms, m_base, m_aux, s_isLoadingShaders);
    }
private:
    const char* m_stage;
    uint64 m_base, m_aux;
    bool m_enabled{};
    std::chrono::steady_clock::time_point m_start{};
};

void ReportShaderLog(GLuint shader, uint64 base, uint64 aux)
{
	BuildTimer timer("compile-log-query", base, aux);
	GLint length = 0;
	glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
	if (length > 0)
	{
		char log[2048]{};
		GLsizei size = 0;
		glGetShaderInfoLog(shader, std::min<uint32>(length, sizeof(log) - 1), &size, log);
		cemuLog_log(LogType::Force, "Error/Warning in shader:");
		cemuLog_log(LogType::Force, log);
	}
}
}

bool RendererShaderGL::loadBinary()
{
	BuildTimer timer("binary-load", m_baseHash, m_auxHash);
	if (!s_programBinaryCache)
		return false;
	if (m_isGameShader == false || m_isGfxPackShader)
		return false; // only non-custom 
	if (!glProgramBinary)
		return false; // OpenGL program binaries not supported

	cemu_assert_debug(m_baseHash != 0);
	uint64 h1, h2;
	GenerateShaderPrecompiledCacheFilename(m_type, m_baseHash, m_auxHash, h1, h2);
	std::vector<uint8> cacheFileData;
	if (!s_programBinaryCache->GetFile({h1, h2 }, cacheFileData))
		return false;
	if (cacheFileData.size() <= sizeof(uint32))
		return false;

	uint32 shaderBinFormat = *(uint32*)(cacheFileData.data());

	m_program = glCreateProgram();
	glProgramBinary(m_program, shaderBinFormat, cacheFileData.data()+4, cacheFileData.size()-4);

	int status = -1;
	glGetProgramiv(m_program, GL_LINK_STATUS, &status);
	if (status != GL_TRUE)
	{
		glDeleteProgram(m_program);
		m_program = 0;
		return false;
	}
	m_isCompiled = true;
	return true;
}

void RendererShaderGL::storeBinary()
{
	BuildTimer timer("binary-export", m_baseHash, m_auxHash);
	if (!s_programBinaryCache)
		return;
	if (!glGetProgramBinary)
		return;
	if (m_program == 0)
		return;
	if (!m_isGameShader || m_isGfxPackShader)
		return;

	GLint binaryLength = 0;
	glGetProgramiv(m_program, GL_PROGRAM_BINARY_LENGTH, &binaryLength);
	if (binaryLength > 0)
	{
		uint64 h1, h2;
		GenerateShaderPrecompiledCacheFilename(m_type, m_baseHash, m_auxHash, h1, h2);
		// build stored shader data (4 byte format + binary data)
		std::vector<uint8> storedBinary(binaryLength+sizeof(uint32), 0);
		GLenum binaryFormat = 0;
		glGetProgramBinary(m_program, binaryLength, NULL, &binaryFormat, storedBinary.data()+sizeof(uint32));		
		*(uint32*)(storedBinary.data() + 0) = binaryFormat;
		// store
		s_programBinaryCache->AddFileAsync({h1, h2 }, storedBinary.data(), storedBinary.size());
	}
}

RendererShaderGL::RendererShaderGL(ShaderType type, uint64 baseHash, uint64 auxHash, bool isGameShader, bool isGfxPackShader, const std::string& glslSource)
	: RendererShader(type, baseHash, auxHash, isGameShader, isGfxPackShader), m_program(0), m_shader_object(0), m_glslSource(glslSource)
{
	BuildTimer timer("construct", baseHash, auxHash);
	if (std::getenv("CEMU_RE_GL_SOURCE_AUDIT") && isGameShader && !isGfxPackShader &&
		RevelationsShaderScheduling::Matches(CafeSystem::GetForegroundTitleId(),
			CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated()))
	{
		// Audit the actual submitted GLSL without enabling the codegen-changing shader-dump option.
		const auto folder = ActiveSettings::GetUserDataPath("revelations-source-audit");
		std::error_code error;
		std::filesystem::create_directories(folder, error);
		if (!error)
		{
			const auto path = folder / fmt::format("{}_{:016x}_{:016x}.glsl", static_cast<int>(type), baseHash, auxHash);
			if (auto* file = FileStream::createFile2(path))
			{
				file->writeData(glslSource.data(), glslSource.size());
				delete file;
			}
		}
	}
	GLenum glShaderType;
	switch (type)
	{
	case ShaderType::kVertex:
		glShaderType = GL_VERTEX_SHADER;
		break;
	case ShaderType::kFragment:
		glShaderType = GL_FRAGMENT_SHADER;
		break;
	case ShaderType::kGeometry:
		glShaderType = GL_GEOMETRY_SHADER;
		break;
	default:
		cemu_assert_debug(false);
	}

	const bool lateBinaryLookup = RevelationsShaderScheduling::LateBinaryLookup(
		s_isLoadingShaders, isGameShader, isGfxPackShader, CafeSystem::GetForegroundTitleId(),
		CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated());
	if (s_isLoadingShaders || lateBinaryLookup)
	{
		if (loadBinary())
		{
			if (lateBinaryLookup)
				cemuLog_logOnce(LogType::Force, "Revelations: reused a cached OpenGL program requested after startup");
			m_glslSource.clear();
			m_glslSource.shrink_to_fit();
			return;
		}
	}

	if (isGameShader && !isGfxPackShader && !s_isLoadingShaders && !s_creatingEarlyProgram &&
		CommandLookaheadEnabled())
	{
		for (auto it = s_earlyPrograms.begin(); it != s_earlyPrograms.end(); ++it)
		{
			if (!RevelationsEarlyPreparation::SameSource(static_cast<unsigned>(it->shader->GetType()), it->source,
				static_cast<unsigned>(type), glslSource)) continue;
			const auto submitted = it->submitted;
			const auto useStarted = std::chrono::steady_clock::now();
			m_program = std::exchange(it->shader->m_program, 0);
			m_shader_object = std::exchange(it->shader->m_shader_object, 0);
			m_shader_attached = std::exchange(it->shader->m_shader_attached, false);
			m_deferredPreparation = true;
			s_earlyPrograms.erase(it);
			// Validation still blocks if the prediction was late. Failure uses the unchanged source path.
			if (WaitForCompiled())
			{
				++s_earlyUsed;
				cemuLog_log(LogType::Force, "[RE:EARLY] event=use base={:016x} aux={:016x} lead_ms={:.3f} wait_ms={:.3f}",
					baseHash, auxHash, std::chrono::duration<double, std::milli>(useStarted - submitted).count(),
					std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - useStarted).count());
				m_glslSource.clear();
				m_glslSource.shrink_to_fit();
				return;
			}
			CleanupShaderObj();
			glDeleteProgram(m_program);
			m_program = 0;
			m_isCompiled = false;
			m_deferredPreparation = false;
			cemuLog_log(LogType::Force, "[RE:EARLY] event=fallback reason=validation base={:016x}", baseHash);
			break;
		}
	}

	m_deferredPreparation = RevelationsGLPreparation::QueueBeforeWait(
		true, s_isLoadingShaders, isGameShader, isGfxPackShader,
		CafeSystem::GetForegroundTitleId(), CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated());
	if (s_creatingEarlyProgram) m_deferredPreparation = true;
	m_shader_object = glCreateShader(glShaderType);

	const char *c_str = m_glslSource.c_str();
	const GLint size = (GLint)m_glslSource.size();

	glShaderSource(m_shader_object, 1, &c_str, &size);
	glCompileShader(m_shader_object);

	if (!m_deferredPreparation)
		ReportShaderLog(m_shader_object, m_baseHash, m_auxHash);

	// set debug name
	if (LaunchSettings::NSightModeEnabled()) 
	{
		auto objNameStr = fmt::format("shader_{:016x}_{:016x}", m_baseHash, m_auxHash);
		glObjectLabel(GL_SHADER, m_shader_object, objNameStr.size(), objNameStr.c_str());
	}

	m_program = glCreateProgram();
	glProgramParameteri(m_program, GL_PROGRAM_SEPARABLE, GL_TRUE);
	glProgramParameteri(m_program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
	glAttachShader(m_program, m_shader_object);
	m_shader_attached = true;
	glLinkProgram(m_program);

	// The existing loading queue waits for every program before gameplay starts.
	if (!m_deferredPreparation)
		storeBinary();

	// count shader compilation
	if (!s_isLoadingShaders && !s_creatingEarlyProgram)
		++g_compiled_shaders_total;

	// we can throw away the GLSL code to conserve RAM
	m_glslSource.clear();
	m_glslSource.shrink_to_fit();
}

RendererShaderGL::~RendererShaderGL()
{
	CleanupShaderObj();

	if (m_program != 0)
		glDeleteProgram(m_program);
}

bool RendererShaderGL::QueueEarlyFragment(uint64 baseHash, uint64 auxHash, const std::string& source)
{
	const auto limit = RevelationsCommandLookahead::MaxPrograms;
	if (!s_parallelCompletionQueries || s_earlyPrograms.size() >= limit)
		return false;
	for (const auto& entry : s_earlyPrograms)
		if (RevelationsEarlyPreparation::SameSource(static_cast<unsigned>(entry.shader->GetType()), entry.source,
			static_cast<unsigned>(ShaderType::kFragment), source)) return false;
	s_creatingEarlyProgram = true;
	auto shader = std::make_unique<RendererShaderGL>(ShaderType::kFragment, baseHash, auxHash, false, false, source);
	s_creatingEarlyProgram = false;
	s_earlyPrograms.push_back({baseHash, auxHash, source, std::move(shader), std::chrono::steady_clock::now()});
	++s_earlySubmitted;
	cemuLog_log(LogType::Force, "[RE:EARLY] event=submit base={:016x} aux={:016x} outstanding={}", baseHash, auxHash, s_earlyPrograms.size());
	return true;
}

bool RendererShaderGL::EarlyFragmentSlotAvailable()
{
	return s_earlyPrograms.size() < RevelationsCommandLookahead::MaxPrograms;
}

bool RendererShaderGL::CommandLookaheadEnabled()
{
	return !s_isLoadingShaders && RevelationsEarlyPreparation::Enabled(
		std::getenv("CEMU_RE_DISABLE_PREPARATION") == nullptr, true, s_parallelCompletionQueries,
		CafeSystem::GetForegroundTitleId(), CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated());
}

bool RendererShaderGL::HasCommandFragment(uint64 base, uint64 aux)
{
	return std::any_of(s_earlyPrograms.begin(), s_earlyPrograms.end(), [=](const auto& entry)
		{ return entry.base == base && entry.aux == aux; });
}

bool RendererShaderGL::BeginCommandFragment(uint64 base, uint64 aux, const std::string& source)
{
	if (!CommandLookaheadEnabled()) return false;
	for (const auto& entry : s_earlyPrograms)
		if (entry.source == source) return false;
	// A new miss starts a bounded batch. Unused guesses never enter a persistent cache.
	ClearEarlyPrograms();
	return QueueEarlyFragment(base, aux, source);
}

void RendererShaderGL::ClearEarlyPrograms()
{
	if (s_earlySubmitted)
		cemuLog_log(LogType::Force, "[RE:EARLY] event=summary submitted={} used={} unused={}",
			s_earlySubmitted, s_earlyUsed, s_earlyPrograms.size());
	s_earlyPrograms.clear();
	s_earlySubmitted = s_earlyUsed = 0;
}

void RendererShaderGL::CleanupShaderObj()
{
	if (m_shader_object != 0 && m_shader_attached)
		glDetachShader(m_program, m_shader_object);

	if (m_shader_object != 0)
		glDeleteShader(m_shader_object);

	m_shader_object = 0;
}

void RendererShaderGL::PreponeCompilation(bool isRenderThread)
{
	// the logic for initiating compilation is currently in the constructor
	// here we only guarantee that it is finished before we return
	if (m_isCompiled)
		return;
	WaitForCompiled();
}

bool RendererShaderGL::IsCompiled()
{
	cemu_assert_debug(false);
	return true;
}

bool RendererShaderGL::WaitForCompiled()
{
	BuildTimer timer("validate", m_baseHash, m_auxHash);
	char infoLog[8 * 1024];
	if (m_isCompiled)
		return true;
	// check if compilation was successful
	GLint compileStatus = GL_FALSE;
	{
		BuildTimer query("compile-status", m_baseHash, m_auxHash);
		glGetShaderiv(m_shader_object, GL_COMPILE_STATUS, &compileStatus);
	}
	if (compileStatus == 0)
	{
		uint32 infoLogLength, tempLength;
		glGetShaderiv(m_shader_object, GL_INFO_LOG_LENGTH, (GLint *)&infoLogLength);
		if (infoLogLength != 0)
		{
			tempLength = sizeof(infoLog) - 1;
			glGetShaderInfoLog(m_shader_object, std::min(infoLogLength, tempLength), (GLsizei*)&tempLength, (GLcharARB*)infoLog);
			infoLog[tempLength] = '\0';
			cemuLog_log(LogType::Force, "Compile error in shader. Log:");
			cemuLog_log(LogType::Force, infoLog);
		}
		m_isCompiled = true;
		CleanupShaderObj();
		return false;
	}
	// get shader binary
	GLint linkStatus = GL_FALSE;
	{
		BuildTimer query("link-status", m_baseHash, m_auxHash);
		glGetProgramiv(m_program, GL_LINK_STATUS, &linkStatus);
	}
	if (linkStatus == 0)
	{
		uint32 infoLogLength, tempLength;
		glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, (GLint *)&infoLogLength);
		if (infoLogLength != 0)
		{
			tempLength = sizeof(infoLog) - 1;
			glGetProgramInfoLog(m_program, std::min(infoLogLength, tempLength), (GLsizei*)&tempLength, (GLcharARB*)infoLog);
			infoLog[tempLength] = '\0';
			cemuLog_log(LogType::Force, "Link error in shader. Log:");
			cemuLog_log(LogType::Force, infoLog);
		}
		m_isCompiled = true;
		CleanupShaderObj();
		return false;
	}

	if (m_deferredPreparation)
	{
		ReportShaderLog(m_shader_object, m_baseHash, m_auxHash);
		storeBinary();
		m_deferredPreparation = false;
	}
	m_isCompiled = true;
	CleanupShaderObj();
	return true;
}

sint32 RendererShaderGL::GetUniformLocation(const char* name)
{
	return glGetUniformLocation(m_program, name);
}

void RendererShaderGL::SetUniform1i(sint32 location, sint32 value)
{
	glProgramUniform1i(m_program, location, value);
}

void RendererShaderGL::SetUniform1f(sint32 location, float value)
{
	glProgramUniform1f(m_program, location, value);
}

void RendererShaderGL::SetUniform2fv(sint32 location, void* data, sint32 count)
{
	glProgramUniform2fv(m_program, location, count, (const GLfloat*)data);
}

void RendererShaderGL::SetUniform4iv(sint32 location, void* data, sint32 count)
{
	glProgramUniform4iv(m_program, location, count, (const GLint*)data);
}

void RendererShaderGL::ShaderCacheLoading_begin(uint64 cacheTitleId)
{
	ClearEarlyPrograms();
	s_parallelCompletionQueries = false;
	if (glGetStringi && RevelationsShaderScheduling::Matches(CafeSystem::GetForegroundTitleId(),
		CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated()))
	{
		GLint count = 0;
		glGetIntegerv(GL_NUM_EXTENSIONS, &count);
		for (GLint i = 0; i < count; ++i)
		{
			const auto name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
			if (name && (!std::strcmp(name, "GL_ARB_parallel_shader_compile") || !std::strcmp(name, "GL_KHR_parallel_shader_compile")))
				s_parallelCompletionQueries = true;
		}
	}
    cemu_assert_debug(!s_programBinaryCache); // should not be set, ShaderCacheLoading_Close() not called?
	// determine if cache is enabled
	bool usePrecompiled = false;
	switch (ActiveSettings::GetPrecompiledShadersOption())
	{
	case PrecompiledShaderOption::Auto:
		if (g_renderer->GetVendor() == GfxVendor::Nvidia)
			usePrecompiled = false;
		else
			usePrecompiled = true;
		break;
	case PrecompiledShaderOption::Enable:
		usePrecompiled = true;
		break;
	case PrecompiledShaderOption::Disable:
		usePrecompiled = false;
		break;
	default:
		UNREACHABLE;
	}

	if (CafeSystem::GetForegroundTitleId() == 0x000500001012cf00ull &&
		CafeSystem::GetForegroundTitleVersion() == 16 && CafeSystem::GetRPXHashUpdated() == 0xefa75829)
		usePrecompiled = true;
	cemuLog_log(LogType::Force, "Using precompiled shaders: {}", usePrecompiled ? "true" : "false");

	if (usePrecompiled)
	{
		const uint32 cacheMagic = GeneratePrecompiledCacheId();
		const std::string cacheFilename = fmt::format("{:016x}_gl.bin", cacheTitleId);
        s_programBinaryCache = FileCache::Open(ActiveSettings::GetCachePath("shaderCache/precompiled/{}", cacheFilename), true, cacheMagic);
		if (s_programBinaryCache == nullptr)
			cemuLog_log(LogType::Force, "Unable to open OpenGL precompiled cache {}", cacheFilename);
	}
	s_isLoadingShaders = true;
}

void RendererShaderGL::ShaderCacheLoading_end()
{
	s_isLoadingShaders = false;
}

void RendererShaderGL::ShaderCacheLoading_Close()
{
	ClearEarlyPrograms();
    if(s_programBinaryCache)
    {
        delete s_programBinaryCache;
        s_programBinaryCache = nullptr;
    }
    g_compiled_shaders_total = 0;
    g_compiled_shaders_async = 0;
}

FileCache* RendererShaderGL::s_programBinaryCache{};
bool RendererShaderGL::s_parallelCompletionQueries{};
