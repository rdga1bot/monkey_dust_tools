#include "flare_demo_internal.h"

void GenCamIcon(uint8_t* p, bool rec) {
    const int W = BTN_SZ;
    auto px = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x<0||x>=W||y<0||y>=W) return;
        int i = (y*W+x)*4; p[i]=r; p[i+1]=g; p[i+2]=b; p[i+3]=255;
    };
    auto rect = [&](int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
        for (int py=y; py<y+h; ++py)
        for (int px_=x; px_<x+w; ++px_) px(px_, py, r, g, b);
    };
    auto circ = [&](int cx, int cy, int rad, uint8_t r, uint8_t g, uint8_t b) {
        for (int py=cy-rad; py<=cy+rad; ++py)
        for (int px_=cx-rad; px_<=cx+rad; ++px_)
            if ((px_-cx)*(px_-cx)+(py-cy)*(py-cy) <= rad*rad)
                px(px_, py, r, g, b);
    };
    // Background (rounded feel via dark corners)
    rect(0, 0, W, W, 30, 30, 40);
    rect(3, 3, W-6, W-6, 45, 45, 55);
    // Camera body (scaled from 56px design × 72/56)
    rect(12, 26, 49, 31, 195, 195, 205);
    // Viewfinder notch
    rect(22, 17, 18, 12, 195, 195, 205);
    // Lens rings
    circ(36, 41, 13, 55, 55, 65);
    circ(36, 41,  9, 195, 195, 205);
    circ(36, 41,  5, 55, 55, 65);
    circ(36, 41,  3, 100, 130, 155);
    if (rec) {
        // Red dot indicator (top-right)
        circ(59, 12,  8, 180, 18, 18);
        circ(59, 12,  5, 235, 55, 55);
        // Red border (3 px)
        for (int i = 0; i < W; ++i) {
            px(i,0,195,20,20); px(i,1,195,20,20); px(i,2,195,20,20);
            px(i,W-1,195,20,20); px(i,W-2,195,20,20); px(i,W-3,195,20,20);
            px(0,i,195,20,20); px(1,i,195,20,20); px(2,i,195,20,20);
            px(W-1,i,195,20,20); px(W-2,i,195,20,20); px(W-3,i,195,20,20);
        }
    } else {
        // Subtle white border when idle
        for (int i = 0; i < W; ++i) {
            px(i,0,80,80,90); px(i,W-1,80,80,90);
            px(0,i,80,80,90); px(W-1,i,80,80,90);
        }
    }
}

// Create a BTN_SZ×BTN_SZ RGBA8 SDL_GPU texture from pixel data.
bool MakeCamTex(md::GpuDeviceHandle dev, const uint8_t* pixels, GpuColorTexture& out) {
    return MdUploadSquareRGBA8ToGpuColorTexture(dev, pixels, BTN_SZ, out);
}

void StartRecording() {
    if (s_recording) return;
#ifndef _WIN32
    pid_t pid = fork();
    if (pid == 0) {
        // New session: detach from the demo's controlling terminal so that
        // grandchild processes (ffmpeg) can't accidentally freeze the shell.
        setsid();
        // Silence stderr (ffmpeg/python noise). Keep stdout for the banner.
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 2); close(devnull); }
        execlp("python3", "python3", "scripts/demo_capture.py",
               "--no-launch", "--record-fps", "10", nullptr);
        _exit(1);
    } else if (pid > 0) {
        s_rec_pid = pid;
        s_recording = true;
        fprintf(stderr, "[rec] Recording started (pid=%d)\n", (int)pid);
    }
#endif
}

// Non-blocking stop — safe to call during the game loop.
// Sends SIGTERM and does a non-blocking WNOHANG poll.
// If the child hasn't exited yet, saves the PID in s_rec_pid_pending
// so WaitRecordingChild() can do a blocking reap at shutdown.
void StopRecording() {
    if (!s_recording) return;
#ifndef _WIN32
    if (s_rec_pid > 0) {
        kill(s_rec_pid, SIGTERM);
        int st = 0;
        pid_t r = waitpid(s_rec_pid, &st, WNOHANG);
        if (r == s_rec_pid) {
            s_rec_pid = -1;          // already exited — nothing to reap at shutdown
        } else {
            s_rec_pid_pending = s_rec_pid;  // still running — block at shutdown
            s_rec_pid = -1;
        }
    }
    s_recording = false;
    fprintf(stderr, "[rec] Recording stopped\n");
#endif
}

// Blocking reap of any pending recording child.
// Call once at shutdown — blocks until demo_capture.py finishes encoding
// and frame extraction so the banner prints before the shell prompt returns.
void WaitRecordingChild() {
#ifndef _WIN32
    if (s_rec_pid_pending > 0) {
        waitpid(s_rec_pid_pending, nullptr, 0);
        s_rec_pid_pending = -1;
    }
#endif
}

// ── World 3D renderer (Step 4) ────────────────────────────────────────────────
// Toggle 2D / 3D view with Tab.
// In 3D mode: orbit camera (arrow keys = rotate, scroll = zoom).



// Terrain

// Geometry scratch — static (BSS), not on stack.

