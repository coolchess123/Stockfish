/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2025 The Stockfish developers (see AUTHORS file)
*/

#include "timeman.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include "search.h"
#include "ucioption.h"

namespace Stockfish {

// Precomputed constants to avoid repeated calculations
constexpr double LOG_10_1000 = std::log10(1000.0);
constexpr double BASE_OPT_CONSTANT = 0.0032116;
constexpr double OPT_CONSTANT_FACTOR = 0.000321123;
constexpr double MAX_OPT_CONSTANT = 0.00508017;
constexpr double BASE_MAX_CONSTANT = 3.3977;
constexpr double MAX_CONSTANT_FACTOR = 3.03950;
constexpr double MIN_MAX_CONSTANT = 2.94761;

// Cache frequently used values
struct TimeConstants {
    double optConstant;
    double maxConstant;
    double timeLeftFactor;
    
    TimeConstants(double logTimeInSec, TimePoint timeLeft, TimePoint totalTime) {
        optConstant = std::min(BASE_OPT_CONSTANT + OPT_CONSTANT_FACTOR * logTimeInSec, 
                              MAX_OPT_CONSTANT);
        maxConstant = std::max(BASE_MAX_CONSTANT + MAX_CONSTANT_FACTOR * logTimeInSec, 
                              MIN_MAX_CONSTANT);
        timeLeftFactor = static_cast<double>(totalTime) / timeLeft;
    }
};

// Remove 'inline' keyword to ensure proper linkage
TimePoint TimeManagement::optimum() const { return optimumTime; }
TimePoint TimeManagement::maximum() const { return maximumTime; }

void TimeManagement::clear() {
    availableNodes = -1;
}

void TimeManagement::advance_nodes_time(std::int64_t nodes) {
    assert(useNodesTime);
    availableNodes = std::max(int64_t(0), availableNodes - nodes);
}

void TimeManagement::init(Search::LimitsType& limits,
                         Color               us,
                         int                 ply,
                         const OptionsMap&   options,
                         double&             originalTimeAdjust) {
    const TimePoint npmsec = TimePoint(options["nodestime"]);
    startTime = limits.startTime;
    useNodesTime = npmsec != 0;

    if (limits.time[us] == 0)
        return;

    const TimePoint moveOverhead = TimePoint(options["Move Overhead"]);
    const int64_t scaleFactor = useNodesTime ? npmsec : 1;

    // Handle nodes time mode initialization
    if (useNodesTime) {
        if (availableNodes == -1)
            availableNodes = npmsec * limits.time[us];
        
        limits.time[us] = TimePoint(availableNodes);
        limits.inc[us] *= npmsec;
        limits.npmsec = npmsec;
    }

    const TimePoint scaledTime = limits.time[us] / scaleFactor;

    // Improved move horizon calculation
    int centiMTG = limits.movestogo ? 
        std::min(limits.movestogo * 100, 5000) : 
        5051;

    // More aggressive time reduction for very short time controls
    if (scaledTime < 1000) {
        centiMTG = std::min(centiMTG, static_cast<int>(scaledTime * 5.051));
    }

    // Enhanced timeLeft calculation with safety margin
    const TimePoint timeLeft = std::max(TimePoint(1),
        limits.time[us] + (limits.inc[us] * (centiMTG - 100) - 
        moveOverhead * (200 + centiMTG)) / 100);

    // Adaptive time management based on game phase
    double optScale, maxScale;
    if (!limits.movestogo) {
        // Initialize originalTimeAdjust if needed
        if (originalTimeAdjust < 0)
            originalTimeAdjust = 0.3128 * std::log10(timeLeft) - 0.4354;

        // Calculate time constants using cached values
        const TimeConstants tc(std::log10(scaledTime) - LOG_10_1000, timeLeft, limits.time[us]);

        // Enhanced time allocation formula for middle and endgame
        optScale = std::min(0.0121431 + std::pow(ply + 2.94693, 0.461073) * tc.optConstant,
                           0.213035 * tc.timeLeftFactor) * originalTimeAdjust;
        
        maxScale = std::min(6.67704, tc.maxConstant + ply / 11.9847);
    }
    else {
        // Simplified calculation for fixed move time controls
        const double movesToGo = centiMTG / 100.0;
        optScale = std::min((0.88 + ply / 116.4) / movesToGo, 
                           0.88 * limits.time[us] / timeLeft);
        maxScale = 1.3 + 0.11 * movesToGo;
    }

    // Calculate final time bounds with dynamic adjustment
    optimumTime = TimePoint(optScale * timeLeft);
    maximumTime = TimePoint(std::min(0.825179 * limits.time[us] - moveOverhead, 
                                    maxScale * optimumTime)) - 10;

    // Hard cap on maximum time usage
    const TimePoint maxCap = TimePoint(0.20 * limits.time[us]);
    maximumTime = std::max(TimePoint(1), std::min(maximumTime, maxCap));

    // Ponder time adjustment
    if (options["Ponder"])
        optimumTime += optimumTime / 4;
}

}  // namespace Stockfish
