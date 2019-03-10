/*
 * ace_artillerytables.cpp
 * Author: PabstMirror
 */

 //#define TEST_EXE

#define _USE_MATH_DEFINES
#include <cmath>
#include <vector>
#include <string>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <tuple>
#include <algorithm>
#include <chrono>
#include <future>

 // ace libs:
#include "vector.hpp"

#ifndef TEST_EXE
extern "C" {
    __declspec(dllexport) void __stdcall RVExtension(char* output, int outputSize, const char* function);
    __declspec(dllexport) int __stdcall RVExtensionArgs(char* output, int outputSize, const char* function, const char** argv, int argc);
    __declspec(dllexport) void __stdcall RVExtensionVersion(char* output, int outputSize);
}
#endif

// Constants
static const double timeStep = 1.0 / 100;
static const double rangeSearchErrorMax = 0.001; // ratio * distance
static const double rangeSearchAngleConvergance = 0.00001;
static const double gravityABS = 9.8066;
static const ace::vector3<double> gravityAccl(0, 0, -1 * gravityABS);

// Globals:
std::vector<std::future<std::string>> fWorkers;
unsigned int getLineIndex = 0;

std::tuple<double, double, double> simulateShot(const double& _fireAngleRad, const double& _muzzleVelocity, const double& _heightOfTarget, const double& _crossWind, const double& _tailWind, const double& _temperature, const double& _airDensity, double _airFriction) {
    const double kCoefficient = -1.0 * _airDensity * _airFriction;
    const double powderEffects = (_airFriction) ? ((_temperature + 273.13) / 288.13 - 1) / 40 + 1 : 1.0;

    double currentTime = 0;
    ace::vector3<double> currentPosition(0, 0, 0);
    ace::vector3<double> lastPosition(currentPosition);
    ace::vector3<double> currentVelocity(0, powderEffects * _muzzleVelocity * cos(_fireAngleRad), powderEffects * _muzzleVelocity * sin(_fireAngleRad));
    const ace::vector3<double> wind(_crossWind, _tailWind, 0);

    while ((currentVelocity.z() > 0) || (currentPosition.z() >= _heightOfTarget)) {
        lastPosition = currentPosition;
        ace::vector3<double> apparentWind = wind - currentVelocity;
        ace::vector3<double> changeInVelocity = gravityAccl + apparentWind * (kCoefficient * apparentWind.magnitude());
        currentVelocity += changeInVelocity * timeStep;
        currentPosition += currentVelocity * timeStep;
        currentTime += timeStep;
    }

    const double lastCurrentRatio((_heightOfTarget - currentPosition.z()) / (lastPosition.z() - currentPosition.z()));
    ace::vector3<double> finalPos = lastPosition.lerp(currentPosition, lastCurrentRatio);

    return { finalPos.x(), finalPos.y(), currentTime };
}

std::tuple<double, double> findMaxAngle(const double& _muzzleVelocity, const double& _airFriction) {
    if (_airFriction == 0) {
        return { M_PI_4, _muzzleVelocity * _muzzleVelocity / gravityABS };
    }
    double bestAngle = M_PI_4;
    double bestDistance = -1;
    double testResultDist;
    for (double testAngle = M_PI_4; testAngle > 0; testAngle -= (M_PI_4 / 100.0)) {
        std::tie(std::ignore, testResultDist, std::ignore) = simulateShot(testAngle, _muzzleVelocity, 0, 0, 0, 15, 1, _airFriction);
        if (testResultDist > bestDistance) {
            bestAngle = testAngle;
            bestDistance = testResultDist;
        }
    }
    return { bestAngle, bestDistance };
}

std::tuple<double, double, double> simulateFindSolution(const double& _rangeToHit, const double& _heightToHit, const double& _muzzleVelocity, const double& _airFriction, const double& _minElev, const double& _maxElev, const bool& _highArc) {
    // returns: actual distance traveled, elevation, time of flight
    double searchMin = _minElev;
    double searchMax = _maxElev;

    if (!_airFriction) {
        // can do trivial ballistics physics to get angle, could compute tof as well, but run through sim once to ensure consistancy (uses positive value of g)
        double radicand = pow(_muzzleVelocity, 4) - gravityABS * (gravityABS * pow(_rangeToHit, 2) + 2 * _heightToHit * pow(_muzzleVelocity, 2));
        if ((!_rangeToHit) || (radicand < 0)) { // radican't
            return { -1, -1, -1 };
        }
        radicand = sqrt(radicand);
        double angleRoot = atan((pow(_muzzleVelocity, 2) + radicand) / (gravityABS * _rangeToHit));
        if ((angleRoot > _maxElev) || (angleRoot < _minElev)) {
            angleRoot = atan((pow(_muzzleVelocity, 2) - radicand) / (gravityABS * _rangeToHit));
        }
        if ((angleRoot > _maxElev) || (angleRoot < _minElev)) {
            return { -1, -1, -1 };
        };
        const double tof = _rangeToHit / (_muzzleVelocity * cos(angleRoot));
        return { _rangeToHit, angleRoot, tof };
    }


    int numberOfAttempts = 0;
    double resultDistance = -1;
    double resultTime = -1;
    double currentError = 9999;
    double currentElevation = -1;
    do {
        if (numberOfAttempts++ > 50) break; // for safetey, min/max should converge long before
        currentElevation = (searchMin + searchMax) / 2.0;
        std::tie(std::ignore, resultDistance, resultTime) = simulateShot(currentElevation, _muzzleVelocity, _heightToHit, 0, 0, 15, 1, _airFriction);
        currentError = _rangeToHit - resultDistance;
        // printf("elev %f [%f, %f]range%f\n goes %f [%f]\n", currentElevation, searchMin, searchMax, (searchMax - searchMin), resultDistance, currentError);
        if ((currentError > 0) ^ (!_highArc)) {
            searchMax = currentElevation;
        } else {
            searchMin = currentElevation;
        }
    } while ((searchMax - searchMin) > rangeSearchAngleConvergance);

    // printf("[%f, %f] Actuall [%f] err [%f of %f]\n", _rangeToHit, _heightToHit, resultDistance, currentError, (_rangeToHit * rangeSearchErrorMax * (_highArc ? 1.0 : 2.0)));
    // On some low angle shots, it will really struggle to converge because of precision issues
    if ((abs(currentError) > (_rangeToHit * rangeSearchErrorMax * (_highArc ? 1.0 : 2.0)))) {
        return { -1, -1, -1 };
    }
    return { resultDistance, currentElevation, resultTime };
}

void writeNumber(std::stringstream & ss, double _num, const int& _widthInt, const int& _widthDec) {
    if ((_num < 0) && (_num > -0.05)) { // hard coded fix -0.0 rounding errors
        _num = 0;
    }
    if (_widthInt > 1) {
        ss << std::setfill('0');
    }
    ss << std::fixed << std::setw(_widthInt) << std::setprecision(_widthDec) << _num;
}

std::string simulateCalcRangeTableLine(const double& _rangeToHit, const double& _muzzleVelocity, const double& _airFriction, const double& _minElev, const double& _maxElev, const bool& _highArc) {
    auto [actualDistance, lineElevation, lineTimeOfFlight] = simulateFindSolution(_rangeToHit, 0, _muzzleVelocity, _airFriction, _minElev, _maxElev, _highArc);
    if (lineTimeOfFlight < 0) {
        return "";
    }
    auto [actualDistanceHeight, lineHeightElevation, lineHeightTimeOfFlight] = simulateFindSolution(_rangeToHit, -100, _muzzleVelocity, _airFriction, _minElev, _maxElev, _highArc);


    std::stringstream returnSS;

    returnSS << "[\"";
    writeNumber(returnSS, _rangeToHit, 0, 0);
    returnSS << "\",\"";
    writeNumber(returnSS, lineElevation * 3200.0 / M_PI, 0, 0);
    returnSS << "\",\"";

    if (lineHeightElevation > 0) {
        const double drElevAdjust = lineHeightElevation - lineElevation;
        const double drTofAdjust = lineHeightTimeOfFlight - lineTimeOfFlight;
        writeNumber(returnSS, drElevAdjust * 3200.0 / M_PI, 0, 0);
        returnSS << "\",\"";
        writeNumber(returnSS, drTofAdjust, 0, 1);
    } else {
        // low angle shots won't be able to adjust down further
        returnSS << "-\",\"-";
    }
    returnSS << "\",\"";
    writeNumber(returnSS, lineTimeOfFlight, 0, ((lineTimeOfFlight < 99.945) ? 1 : 0)); // round TOF when high
    returnSS << "\",\"";

    if (_airFriction) {
        // Calc corrections:
        double xOffset, yOffset;
        // Crosswind
        std::tie(xOffset, std::ignore, std::ignore) = simulateShot(lineElevation, _muzzleVelocity, 0, 10, 0, 15, 1, _airFriction);
        const double crosswindOffsetRad = atan2(xOffset, actualDistance) / 10;
        // Headwind
        std::tie(std::ignore, yOffset, std::ignore) = simulateShot(lineElevation, _muzzleVelocity, 0, 0, -10, 15, 1, _airFriction);
        const double headwindOffset = (actualDistance - yOffset) / 10;
        // Tailwind
        std::tie(std::ignore, yOffset, std::ignore) = simulateShot(lineElevation, _muzzleVelocity, 0, 0, 10, 15, 1, _airFriction);
        const double tailwindOffset = (actualDistance - yOffset) / 10;
        // Air Temp Dec
        std::tie(std::ignore, yOffset, std::ignore) = simulateShot(lineElevation, _muzzleVelocity, 0, 0, 0, 5, 1, _airFriction);
        const double tempDecOffset = (actualDistance - yOffset) / 10;
        // Air Temp Inc
        std::tie(std::ignore, yOffset, std::ignore) = simulateShot(lineElevation, _muzzleVelocity, 0, 0, 0, 25, 1, _airFriction);
        const double tempIncOffset = (actualDistance - yOffset) / 10;
        // Air Density Dec
        std::tie(std::ignore, yOffset, std::ignore) = simulateShot(lineElevation, _muzzleVelocity, 0, 0, 0, 15, 0.9, _airFriction);
        const double airDensityDecOffset = (actualDistance - yOffset) / 10;
        // Air Density Inc
        std::tie(std::ignore, yOffset, std::ignore) = simulateShot(lineElevation, _muzzleVelocity, 0, 0, 0, 15, 1.1, _airFriction);
        const double airDensityIncOffset = (actualDistance - yOffset) / 10;

        writeNumber(returnSS, crosswindOffsetRad * 3200.0 / M_PI, 1, 1);
        returnSS << "\",\"";
        writeNumber(returnSS, headwindOffset, 1, (abs(headwindOffset) > 9.949) ? 0 : 1);
        returnSS << "\",\"";
        writeNumber(returnSS, tailwindOffset, 1, (abs(tailwindOffset) > 9.949) ? 0 : 1);
        returnSS << "\",\"";
        writeNumber(returnSS, tempDecOffset, 1, (abs(tempDecOffset) > 9.949) ? 0 : 1);
        returnSS << "\",\"";
        writeNumber(returnSS, tempIncOffset, 1, (abs(tempIncOffset) > 9.949) ? 0 : 1);
        returnSS << "\",\"";
        writeNumber(returnSS, airDensityDecOffset, 1, (abs(airDensityDecOffset) > 9.949) ? 0 : 1);
        returnSS << "\",\"";
        writeNumber(returnSS, airDensityIncOffset, 1, (abs(airDensityIncOffset) > 9.949) ? 0 : 1);
        returnSS << "\"]";
    } else {
        returnSS << "-\",\"-\",\"-\",\"-\",\"-\",\"-\",\"-\"]"; // 7 dashes
    }
    return (returnSS.str());
}

#ifndef ACE_FULL_VERSION_STR
#define ACE_FULL_VERSION_STR "not defined"
#endif
void RVExtensionVersion(char* output, int outputSize) {
    strncpy_s(output, outputSize, ACE_FULL_VERSION_STR, _TRUNCATE);
}
void RVExtension(char* output, int outputSize, const char* function) {
    if (!strcmp(function, "version")) {
        RVExtensionVersion(output, outputSize);
        return;
    }
    strncpy_s(output, outputSize, "error", _TRUNCATE);
}
int RVExtensionArgs(char* output, int outputSize, const char* function, const char** args, int argsCnt) {
    if (!strcmp(function, "version")) {
        RVExtensionVersion(output, outputSize);
        return 0;
    }

    if (!strcmp(function, "start")) {
        if (argsCnt != 5) { return -2; } // Error: not enough args
        const double muzzleVelocity = strtod(args[0], NULL);
        const double airFriction = strtod(args[1], NULL);
        double minElev = (M_PI / 180.0) * strtod(args[2], NULL);
        double maxElev = (M_PI / 180.0) * strtod(args[3], NULL);
        const bool highArc = !strcmp(args[4], "true");

        // Reset workers:
        fWorkers.clear();
        getLineIndex = 0;

        auto [bestAngle, bestDistance] = findMaxAngle(muzzleVelocity, airFriction);

        minElev = std::max(minElev, 2 * (M_PI / 180.0)); // cap min to 2 degrees (negative elev might get messy)
        maxElev = std::min(maxElev, 88 * (M_PI / 180.0)); // cap max to 88 degrees (mk6)
        if (highArc) {
            minElev = std::max(minElev, bestAngle);
        } else {
            maxElev = std::min(maxElev, bestAngle);
        }
        const double loopStart = (bestDistance < 4000) ? 50 : 100;
        const double loopInc = (bestDistance < 5000) ? 50 : 100; // simplify when range gets high
        const double loopMaxRange = std::min(bestDistance, 25000.0); // with no air resistance, max range could go higher than 60km

        if (maxElev > minElev) { // don't bother if we can't hit anything (e.g. mortar in low mode)
            for (double range = loopStart; range < loopMaxRange; range += loopInc) {
                fWorkers.emplace_back(std::async(&simulateCalcRangeTableLine, range, muzzleVelocity, airFriction, minElev, maxElev, highArc));
            }
        }

        std::stringstream outputStr; // debug max distance and thead count
        outputStr << "[" << bestDistance << "," << fWorkers.size() << "]";
        strncpy_s(output, outputSize, outputStr.str().c_str(), _TRUNCATE);
        return 0;
    }

    if (!strcmp(function, "getline")) {
        // 1 = data on line, 2 - data not ready, 3 - done
        std::string result = "";
        std::future_status workerStatus;

        while (result.empty()) {
            if (getLineIndex >= fWorkers.size()) {
                return 3;
            }
            workerStatus = fWorkers[getLineIndex].wait_for(std::chrono::seconds(0));
            if (workerStatus != std::future_status::ready) {
                return 2;
            }
            result = fWorkers[getLineIndex].get();
            getLineIndex++;
        }
        strncpy_s(output, outputSize, result.c_str(), _TRUNCATE);
        return 1;
    }

    return -1; // Error: function not valid
}


#ifdef TEST_EXE
int main() {

    //double a, b;
    //std::tie(a, b) = simulateFindSolution(200,50, 100, 0, 0, 45 * (M_PI / 180.0), false);
    //printf("sim: %f, %f\n",a,b);

    //std::string r = simulateCalcRangeTableLine(4000, 810, );
    //printf("result: [%s]\n", r.c_str());

    //auto [lineElevation, lineTimeOfFlight] = simulateFindSolution(4000, 0, 810, -0.00005, 5 * (M_PI / 180.0), 80 * (M_PI / 180.0), false);
    //printf("result: [%f, %f]\n", lineElevation, lineTimeOfFlight);


    // Determine realistic air firiction values
    /*
    double mv = 241;
    printf(" %f m/s\n", mv);
    double range;
    for (double ar = 0; ar > -0.00015; ar -= 0.00001) {
        std::tie(std::ignore, range) = findMaxAngle(mv, ar);
        printf("[%f] = %f\n", ar, range);
    }
    */
    /*
    // test callExtension
    char output[256];
    char function1[] = "start";
    //const char* args1[] = { "200", "0", "-5", "80", "false" };
    //const char* args1[] = { "153.9", "-0.00005", "-5", "80", "false" };
    const char* args1[] = { "810", "-0.00005", "-5", "80", "false" };
    //const char* args1[] = { "810", "0", "-5", "80", "true" };
    auto t1 = std::chrono::high_resolution_clock::now();
    int ret = RVExtensionArgs(output, 256, function1, args1, 5);
    auto t2 = std::chrono::high_resolution_clock::now();
    std::printf("ret: %d - %s\n", ret, output);
    std::printf("func %s: %1.1f ms\n", function1, std::chrono::duration<double, std::milli>(t2 - t1).count());

    int lines = 0;
    auto t3 = std::chrono::high_resolution_clock::now();
    char function2[] = "getline";
    int ret2 = 0;
    while (ret2 != 3) { // dumb spin
        ret2 = RVExtensionArgs(output, 256, function2, NULL, 0);
        if (ret2 == 1) {
            lines++;
            //std::printf("ret: %d - %s\n", ret2, output);
        }
    }
    auto t4 = std::chrono::high_resolution_clock::now();
    std::printf("func %s: %1.1f ms with %d lines\n", function2, std::chrono::duration<double, std::milli>(t4 - t3).count(), lines);

    std::printf("callExtensions finished in %1.1f ms\n", std::chrono::duration<double, std::milli>(t4 - t1).count());
    */

}
#endif
