#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/CafeSystem.h"

#include "Cafe/HW/Latte/Core/LatteQueryObject.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"

#define GPU7_QUERY_TYPE_OCCLUSION	(1)

uint64 queryEventCounter = 1;

struct LatteGX2QueryInformation
{
	MPTR queryMPTR;
	uint64 queryEventStart;
	uint64 queryEventEnd;
	uint64 sampleSum;
	HRTick hostEndTick;
	bool queryEnded;
};

std::vector<LatteGX2QueryInformation*> list_activeGX2Queries2;

std::vector<LatteQueryObject*> list_queriesInFlight;

uint64 latestQueryFinishedEventId = 0;

LatteQueryObject* _currentlyActiveRendererQuery = {0};

namespace
{
	bool IsMN9QueryTraceTarget()
	{
		return CafeSystem::GetForegroundTitleId() == 0x00050000101C9600ull &&
			CafeSystem::GetForegroundTitleVersion() == 16 &&
			CafeSystem::GetRPXHashUpdated() == 0x73E4FFC8;
	}

	struct MN9QueryDiagnostics
	{
		HRTick windowStart{};
		uint64 gx2Begins{};
		uint64 gx2Ends{};
		uint64 rendererEnds{};
		uint64 updateCalls{};
		uint64 backendNotReadyChecks{};
		uint64 rendererResults{};
		uint64 gx2Results{};
		uint64 nonzeroResults{};
		uint64 totalCompletionTicks{};
		uint64 maxCompletionTicks{};
		size_t maxActive{};
		size_t maxInFlight{};
	};

	MN9QueryDiagnostics s_mn9QueryDiagnostics;

	void TraceMN9QuerySummary()
	{
		if (!IsMN9QueryTraceTarget())
		{
			s_mn9QueryDiagnostics = {};
			return;
		}

		const HRTick now = HighResolutionTimer::now().getTick();
		auto& trace = s_mn9QueryDiagnostics;
		if (trace.windowStart == 0)
			trace.windowStart = now;
		trace.maxActive = std::max(trace.maxActive, list_activeGX2Queries2.size());
		trace.maxInFlight = std::max(trace.maxInFlight, list_queriesInFlight.size());
		if (HighResolutionTimer::getTimeDiff(trace.windowStart, now) < 1.0)
			return;

		uint64 oldestPendingTicks = 0;
		for (const auto* query : list_activeGX2Queries2)
		{
			if (query->queryEnded && query->hostEndTick != 0)
				oldestPendingTicks = std::max(oldestPendingTicks, now - query->hostEndTick);
		}
		const double averageCompletionMs = trace.gx2Results == 0 ? 0.0 :
			(double)HighResolutionTimer::ticksToMicroseconds(trace.totalCompletionTicks) / (double)trace.gx2Results / 1000.0;
		cemuLog_log(LogType::Force,
			"[COMPAT:MN9:QUERY] layer=core interval_ms={:.1f} gx2_begin={} gx2_end={} renderer_end={} renderer_ready={} gx2_ready={} nonzero={} not_ready_checks={} updates={} active={} inflight={} max_active={} max_inflight={} avg_complete_ms={:.3f} max_complete_ms={:.3f} oldest_pending_ms={:.3f}",
			HighResolutionTimer::getTimeDiff(trace.windowStart, now) * 1000.0,
			trace.gx2Begins, trace.gx2Ends, trace.rendererEnds, trace.rendererResults,
			trace.gx2Results, trace.nonzeroResults, trace.backendNotReadyChecks, trace.updateCalls,
			list_activeGX2Queries2.size(), list_queriesInFlight.size(), trace.maxActive, trace.maxInFlight,
			averageCompletionMs,
			(double)HighResolutionTimer::ticksToMicroseconds(trace.maxCompletionTicks) / 1000.0,
			(double)HighResolutionTimer::ticksToMicroseconds(oldestPendingTicks) / 1000.0);

		trace = {};
		trace.windowStart = now;
	}
}

uint64 LatteQuery_getNextEventId()
{
	uint64 ev = queryEventCounter;
	queryEventCounter++;
	return ev;
}

void LatteQuery_begin(LatteQueryObject* queryObject, uint64 eventId)
{
	queryObject->queryEventStart = eventId;
	queryObject->begin();
}

void LatteQuery_end(LatteQueryObject* queryObject, uint64 eventId)
{
	cemu_assert_debug(!queryObject->queryEnded);
	queryObject->queryEnded = true;
	queryObject->queryEventEnd = eventId;
	queryObject->end();
}

LatteQueryObject* LatteQuery_createSamplePassedQuery()
{
	return g_renderer->occlusionQuery_create();
}

void LatteQuery_finishGX2Query(LatteGX2QueryInformation* gx2Query)
{
	uint32* queryObjectData = (uint32*)memory_getPointerFromVirtualOffset(gx2Query->queryMPTR);
	*(uint64*)(queryObjectData + 0) = 0;
	*(uint64*)(queryObjectData + 2) = gx2Query->sampleSum;
	*(uint64*)(queryObjectData + 4) = 0;
	*(uint64*)(queryObjectData + 6) = 0;

	*(uint64*)(queryObjectData + 8) = 0; // overwrites the 'OCPU' magic constant letting GX2QueryGetOcclusionResult know that the query is finished (for CPU queries)
}

void LatteQuery_UpdateFinishedQueries()
{
	if (IsMN9QueryTraceTarget())
		s_mn9QueryDiagnostics.updateCalls++;
	g_renderer->occlusionQuery_updateState();
	for(uint32 i=0; i<list_queriesInFlight.size(); i++)
	{
		LatteQueryObject* queryObject = list_queriesInFlight[i];
		cemu_assert_debug(queryObject->queryEnded);
		if( queryObject->queryEnded == false )
			continue;
		// check if result is available
		uint64 numSamplesPassed;
		if (!queryObject->getResult(numSamplesPassed))
		{
			if (IsMN9QueryTraceTarget())
				s_mn9QueryDiagnostics.backendNotReadyChecks++;
			break;
		}
		if (IsMN9QueryTraceTarget())
			s_mn9QueryDiagnostics.rendererResults++;

		cemu_assert_debug(latestQueryFinishedEventId < queryObject->queryEventEnd);
		latestQueryFinishedEventId = queryObject->queryEventEnd;

		// add number of passed samples to all gx2 queries that were active at the time
		for (auto& it : list_activeGX2Queries2)
		{
			if (queryObject->queryEventStart >= it->queryEventStart && queryObject->queryEventEnd <= it->queryEventEnd)
				it->sampleSum += numSamplesPassed;
		}

		list_queriesInFlight.erase(list_queriesInFlight.begin() + i);
		i--;
		g_renderer->occlusionQuery_destroy(queryObject);
	}
	// check for finished GX2 queries
	for (sint32 i = 0; i < list_activeGX2Queries2.size(); i++)
	{
		auto gx2Query = list_activeGX2Queries2[i];
		if (gx2Query->queryEnded && latestQueryFinishedEventId >= gx2Query->queryEventEnd)
		{
			if (IsMN9QueryTraceTarget())
			{
				auto& trace = s_mn9QueryDiagnostics;
				trace.gx2Results++;
				trace.nonzeroResults += gx2Query->sampleSum != 0;
				if (gx2Query->hostEndTick != 0)
				{
					const uint64 completionTicks = HighResolutionTimer::now().getTick() - gx2Query->hostEndTick;
					trace.totalCompletionTicks += completionTicks;
					trace.maxCompletionTicks = std::max(trace.maxCompletionTicks, completionTicks);
				}
			}
			LatteQuery_finishGX2Query(gx2Query);
			free(gx2Query);
			list_activeGX2Queries2.erase(list_activeGX2Queries2.begin() + i);
			i--;
		}
	}
	TraceMN9QuerySummary();
}

void LatteQuery_WaitForEndedQueries()
{
	uint64 lastEndedEvent = 0;
	for (const auto* query : list_queriesInFlight)
	{
		if (query->queryEnded)
			lastEndedEvent = std::max(lastEndedEvent, query->queryEventEnd);
	}
	if (lastEndedEvent == 0)
		return;

	// A query may span guest command buffers. Wait only for ended host queries,
	// without ending the active query or publishing a partial guest result.
	g_renderer->occlusionQuery_flush();
	do
	{
		LatteQuery_UpdateFinishedQueries();
	} while (latestQueryFinishedEventId < lastEndedEvent);
}

void LatteQuery_UpdateFinishedQueriesForceFinishAll()
{
	cemu_assert_debug(_currentlyActiveRendererQuery == nullptr);
	g_renderer->occlusionQuery_flush(); // guarantees that all query commands have been submitted and finished processing
	while (true)
	{
		LatteQuery_UpdateFinishedQueries();
		if (list_queriesInFlight.empty())
			break;
	}
}

sint32 checkQueriesCounter = 0;

void LatteQuery_endActiveRendererQuery(uint64 currentEventId)
{
	if (_currentlyActiveRendererQuery != nullptr)
	{
		if (IsMN9QueryTraceTarget())
			s_mn9QueryDiagnostics.rendererEnds++;
		LatteQuery_end(_currentlyActiveRendererQuery, currentEventId);
		list_queriesInFlight.emplace_back(_currentlyActiveRendererQuery);
		_currentlyActiveRendererQuery = nullptr;
	}
}

void LatteQuery_BeginOcclusionQuery(MPTR queryMPTR)
{
	if (IsMN9QueryTraceTarget())
		s_mn9QueryDiagnostics.gx2Begins++;
	if (checkQueriesCounter < 7)
	{
		checkQueriesCounter++;
	}
	else
	{
		LatteQuery_UpdateFinishedQueries();
		checkQueriesCounter = 0;
	}

	for(auto& it : list_activeGX2Queries2)
	{
		if (it->queryMPTR == queryMPTR)
		{
			debug_printf("itHLEBeginOcclusionQuery: Query 0x%08x is already active\n", queryMPTR);
			return;
		}
	}
	uint64 currentEventId = LatteQuery_getNextEventId();
	// end any currently active query
	LatteQuery_endActiveRendererQuery(currentEventId);
	// create GX2 query binding
	LatteGX2QueryInformation* queryBinding = (LatteGX2QueryInformation*)malloc(sizeof(LatteGX2QueryInformation));
	memset(queryBinding, 0x00, sizeof(LatteGX2QueryInformation));
	queryBinding->queryEventStart = currentEventId;
	queryBinding->queryMPTR = queryMPTR;
	list_activeGX2Queries2.emplace_back(queryBinding);
	// start renderer query
	LatteQueryObject* queryObject = LatteQuery_createSamplePassedQuery();
	LatteQuery_begin(queryObject, currentEventId);
	_currentlyActiveRendererQuery = queryObject;
}

void LatteQuery_EndOcclusionQuery(MPTR queryMPTR)
{
	if (queryMPTR == MPTR_NULL)
		return;
	uint64 currentEventId = LatteQuery_getNextEventId();
	// mark query binding as ended
	for(auto& it : list_activeGX2Queries2)
	{
		if (it->queryMPTR == queryMPTR)
		{
			it->queryEventEnd = currentEventId;
			it->hostEndTick = HighResolutionTimer::now().getTick();
			it->queryEnded = true;
			if (IsMN9QueryTraceTarget())
				s_mn9QueryDiagnostics.gx2Ends++;
			break;
		}
	}
	// end currently active renderer query
	LatteQuery_endActiveRendererQuery(currentEventId);
	// check if there are still active GX2 queries
	bool hasActiveGX2Query = false;
	for (auto& it : list_activeGX2Queries2)
	{
		if (!it->queryEnded)
		{
			hasActiveGX2Query = true;
			break;
		}
	}
	// start a new renderer query if there are still active GX2 queries
	if (hasActiveGX2Query)
	{
		LatteQueryObject* queryObject = LatteQuery_createSamplePassedQuery();
		LatteQuery_begin(queryObject, currentEventId);
		const bool isMightyNo9V16 =
			CafeSystem::GetForegroundTitleId() == 0x00050000101C9600ull &&
			CafeSystem::GetForegroundTitleVersion() == 16 &&
			CafeSystem::GetRPXHashUpdated() == 0x73E4FFC8;
		if (!isMightyNo9V16)
		{
			list_queriesInFlight.emplace_back(queryObject);
		}
		// MN9 registers this active query when it ends; inserting it here as well recycles the same object twice.
		_currentlyActiveRendererQuery = queryObject;
		catchOpenGLError();
	}
}

void LatteQuery_CancelActiveGPU7Queries()
{
	cemu_assert_debug(_currentlyActiveRendererQuery == nullptr);
}

void LatteQuery_Init()
{
}
