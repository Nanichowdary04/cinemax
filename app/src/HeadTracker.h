// Webcam head tracking for headphone 3D sound ("the sound stays on the screen").
//
// A worker thread reads the laptop camera through Media Foundation (shared mode, so video
// calls can still use it), finds the face with MediaPipe BlazeFace (ONNX Runtime, loaded at
// run time from onnxruntime.dll next to the exe), and turns the six face points into a head
// yaw / pitch:
//   - roll is removed by rotating the points into the eye line
//   - yaw   = atan(nose offset from the eye centre / 0.31 eye-distances)   (nose ~2 cm in
//             front of the eyes, eyes ~6.4 cm apart)
//   - pitch from where the nose sits between the eyes and the mouth
//   - the neutral pose is learned automatically (you mostly look at the screen)
//   - a One-Euro filter removes jitter without adding lag
// The pose goes to the audio engine through HeadPose atomics. Nothing is recorded or saved;
// frames live only in memory for the few milliseconds it takes to analyse them.
#pragma once

#include "cinemax/core/Params.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace cinemax
{
namespace head { class FaceAnalyser; }

class HeadTracker
{
public:
    struct Status
    {
        std::string status = "off";   // off | starting | tracking | no-face | no-camera | camera-busy | camera-denied | error
        std::string message;
        float yaw = 0.0f, pitch = 0.0f, fps = 0.0f;
    };

    explicit HeadTracker (HeadPose& pose);
    ~HeadTracker();

    /** Message thread: start or stop the camera (cheap to call repeatedly). */
    void setActive (bool shouldRun);
    bool isActive() const noexcept { return thread.joinable(); }

    /** Mirrored camera picture: report turns the other way. */
    void setFlip (bool f) noexcept { flip = f; }

    Status status() const;

    /** Test hook: runs the face analysis on one BGRA image (top-down), 'frames' times. */
    struct ImageResult
    {
        bool found = false;
        float score = 0, rawYaw = 0, rawPitch = 0, msPerFrame = 0;
        std::string error;
    };
    static ImageResult analyseImage (const std::wstring& dir, const unsigned char* bgra, int width, int height, int frames);

    /** Loads ONNX Runtime and the face model from 'dir' (tests use this directly). */
    static bool loadFaceAnalyser (head::FaceAnalyser& fa, const std::wstring& dir, std::string& error);

private:
    void run();
    void setStatus (const char* s, std::string message = {});

    HeadPose& pose;
    std::thread thread;
    std::atomic<bool> stopFlag { false };
    std::atomic<bool> flip { false };
    mutable std::mutex statusLock;
    Status current;
};

} // namespace cinemax
