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

// Calculate time bounds efficiently with minimal redundant calculations
void calculateTimeBounds(TimePoint& optimumTime, TimePoint& maximumTime,
                        const TimePoint timeLeft, const TimePoint totalTime,
                        const double optScale, const double maxScale,
                        const TimePoint moveOverhead) {
    // Calculate base optimum time once
    optimumTime = TimePoint(optScale * timeLeft);

    // Apply percentage cap of total time (20%)
    // and ensure minimum of 1
    optimumTime = std::max(TimePoint(1),
                          std::min(optimumTime,
                                 TimePoint(0.20 * totalTime)));

    // Calculate maximum time using the capped optimum
    // Use pre-calculated base and scale factors
    const TimePoint baseMaxTime = TimePoint(0.825179 * totalTime - moveOverhead);
    maximumTime = std::min(baseMaxTime,
                          TimePoint(maxScale * optimumTime));

    // Cap maximum time at 30% of total and ensure it's at least
    // equal to optimum time and minimum of 1
    maximumTime = std::max(TimePoint(1),
                          std::max(optimumTime,
                                 std::min(maximumTime,
                                        TimePoint(0.30 * totalTime))));
}

void TimeManagement::init(Search::LimitsType& limits,
                         Color               us,
                         int                 ply,
                         const OptionsMap&   options,
                         double&             originalTimeAdjust) {
    // Early exit if no time control
    startTime = limits.startTime;
    if (limits.time[us] == 0) {
        useNodesTime = false;
        return;
    }

    // Initialize node-based time management
    const TimePoint npmsec = TimePoint(options["nodestime"]);
    useNodesTime = npmsec != 0;
    if (useNodesTime) {
        if (availableNodes == -1) {
            availableNodes = npmsec * limits.time[us];
        }
        limits.time[us] = TimePoint(availableNodes);
        limits.inc[us] *= npmsec;
        limits.npmsec = npmsec;
    }

    // Calculate core time parameters
    const int64_t scaleFactor = useNodesTime ? npmsec : 1;
    const TimePoint moveOverhead = TimePoint(options["Move Overhead"]);
    const TimePoint scaledTime = limits.time[us] / scaleFactor;

    // Calculate moves to go and handle short time controls
    const int centiMTG = [&]() {
        int mtg = limits.movestogo ?
            std::min(limits.movestogo * 100, 5000) :
            5051;
        return (scaledTime < 1000) ?
            std::min(mtg, static_cast<int>(scaledTime * 5.051)) :
            mtg;
    }();

    // Calculate time left with safety margin
    const TimePoint timeLeft = std::max(TimePoint(1),
        limits.time[us] + (limits.inc[us] * (centiMTG - 100) -
        moveOverhead * (200 + centiMTG)) / 100);

    // Calculate scaling factors based on game phase
    const auto [optScale, maxScale] = [&]() -> std::pair<double, double> {
        if (!limits.movestogo) {
            // Update originalTimeAdjust if needed
            if (originalTimeAdjust < 0) {
                originalTimeAdjust = 0.3128 * std::log10(timeLeft) - 0.4354;
            }

            // Calculate time constants
            const double logTimeInSec = std::log10(scaledTime / 1000.0);
            const double optConstant = std::min(0.0032116 + 0.000321123 * logTimeInSec, 0.00508017);
            const double maxConstant = std::max(3.3977 + 3.03950 * logTimeInSec, 2.94761);
            const double timeLeftFactor = static_cast<double>(limits.time[us]) / timeLeft;

            return {
                std::min(0.0121431 + std::pow(ply + 2.94693, 0.461073) * optConstant,
                        0.213035 * timeLeftFactor) * originalTimeAdjust,
                std::min(6.67704, maxConstant + ply / 11.9847)
            };
        } else {
            const double movesToGo = centiMTG / 100.0;
            return {
                std::min((0.88 + ply / 116.4) / movesToGo, 0.88 * limits.time[us] / timeLeft),
                1.3 + 0.11 * movesToGo
            };
        }
    }();

    // Calculate and apply bounds to time allocations
    const auto calculateBounds = [&](TimePoint time, double scale) {
        return std::max(TimePoint(1),
               std::min(TimePoint(scale * limits.time[us]),
                       TimePoint(optScale * timeLeft)));
    };

    optimumTime = calculateBounds(timeLeft, 0.20);
    maximumTime = std::max(optimumTime,
                  std::min(TimePoint(0.30 * limits.time[us]),
                          TimePoint(std::min(0.825179 * limits.time[us] - moveOverhead,
                                           maxScale * optimumTime))));

    // Adjust for pondering
    if (options["Ponder"]) {
        optimumTime += optimumTime / 4;
    }
}

}  // namespace Stockfish
