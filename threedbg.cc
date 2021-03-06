#include "threedbg.h"

#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <set>

#include <thread>
#include <mutex>
#include <condition_variable>

#include "Application.h"

#include "widgets.h"

#define errorfln(fmt, ...) fprintf(stderr, fmt"\n", __VA_ARGS__)

class ThreedbgApp : public Application {
public:
    ThreedbgApp(int width = 1280, int height = 720);
    ~ThreedbgApp();
    void close() {
        Application::close();
        em.setState(ExecuteManager::RUNNING);
    }
    void loopOnce();
    void addDrawer(std::string name, std::unique_ptr<Drawer> d) {
        drawers[name] = std::move(d);
    }
    void snapshot(int & w, int & h, std::vector<unsigned char> & pixels) {
        draw();
        w = cam.resolution[0]; h = cam.resolution[1];
        pixels.resize(w* h * 4);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glCheckError();
    }
    void barrier() {
        em.barrier();
    }
    Camera cam;
    std::vector<std::string> getInvisible() {
        std::vector<std::string> r;
        for (auto &t : invisible)
            if (drawers.find(t) != drawers.end())
                r.push_back(t);
        return r;
    }
    void setInvisible(std::vector<std::string> tl) {
        invisible = std::set<std::string>(tl.begin(), tl.end());
    }
private:
    DrawingCtx ctx;
    ImageViewer iv;
    ExecuteManager em;

    std::map<std::string, struct std::unique_ptr<Drawer>> drawers;
    std::set<std::string> invisible;
    void draw() {
        ctx.bindFB(cam.resolution.x, cam.resolution.y);
        glClearColor(0.5, 0.5, 0.5, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        struct draw_param dp;
        {
            auto mat = cam.getMat();
            memcpy(&dp.mat, &mat, 16 * sizeof(float));
            dp.cam = cam;
        }
        for (auto & d : drawers)
            if (invisible.find(d.first) == invisible.end())
                d.second->draw(dp);
    }
    void ImGuiManipulateCamera() {
        cam.ImGuiDrag();
        cam.ImGuiEdit();
    }
    void ImGuiSwitchDrawers() {
        for (auto & d : drawers) {
            auto it = invisible.find(d.first);
            bool vf = (it == invisible.end());
            if (ImGui::Checkbox(d.first.c_str(), &vf)) {
                if (vf) invisible.erase(it);
                else invisible.insert(d.first);
            }
        }
    }
};

ThreedbgApp::ThreedbgApp(int width, int height) : Application("3D debug", 0, width, height) {
    ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;
    ImGui::StyleColorsLight();
    glEnable(GL_DEPTH_TEST);
    PointsDrawer::initGL();
    LinesDrawer::initGL();
    glCheckError();
}
ThreedbgApp::~ThreedbgApp() {
    LinesDrawer::freeGL();
    PointsDrawer::freeGL();
    glCheckError();
    em.setState(ExecuteManager::RUNNING);
}
void ThreedbgApp::loopOnce() {
    glCheckError();
    Application::newFrame();
    ImVec4 bg_color = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    glClearColor(bg_color.x, bg_color.y, bg_color.z, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    {
        ImGuiIO& io = ImGui::GetIO();
        ImDrawList * drawList = ImGui::GetOverlayDrawList();
        drawList->PushClipRectFullScreen();
        {
            char text[64];
            sprintf(text, "%.1f fps", ImGui::GetIO().Framerate);
            ImVec2 size = ImGui::CalcTextSize(text);
            drawList->AddText(ImVec2(io.DisplaySize.x - size.x - 4, io.DisplaySize.y - size.y - 4), 0xff000000, text);
        }
        drawList->PopClipRect();
    }

    if (ImGui::Begin("drawers")) {
        ImGuiSwitchDrawers();
    }
    ImGui::End();
    if (ImGui::Begin("camera")) {
        ImGuiManipulateCamera();
    }
    ImGui::End();

    draw();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (ImGui::Begin("image")) {
        iv.Show(ctx.texture, ImVec2(ctx.resolution[0], ctx.resolution[1]));
    }
    ImGui::End();

    if (ImGui::Begin("execute")) {
        em.Show();
    }
    ImGui::End();

    //ImGui::ShowDemoWindow();

    Application::endFrame();
    glCheckError();
}

#include <thread>
#include <mutex>
#include <queue>

namespace threedbg {
// basicly ThreedbgApp + thread-safe drawerfactories as cache
bool showGui = true;
static std::thread displayThread;
#ifdef _MSC_VER
class queued_lock {
    std::mutex mtx;
    std::condition_variable cv;
    std::queue<std::thread::id> thread_queue;
    bool locked = false;
public:
    void lock() {
        std::unique_lock<std::mutex> lk(mtx);
        thread_queue.push(std::this_thread::get_id());
        while (locked || std::this_thread::get_id() != thread_queue.front())
            cv.wait(lk);
        thread_queue.pop();
        locked = true;
    }
    void unlock() {
        {
            std::unique_lock<std::mutex> lk(mtx);
            locked = false;
        }
        cv.notify_all();
    }
};
#else
typedef std::mutex queued_lock;
#endif

static queued_lock context_lock;
static queued_lock cache_lock;
static bool allow_free = false;

static std::map<std::string, std::unique_ptr<DrawerFactory>> drawerFactories;
static std::unique_ptr<ThreedbgApp> app = nullptr;

static void flushDrawers() {
    std::map<std::string, std::unique_ptr<DrawerFactory>> dfs = std::move(drawerFactories);
    drawerFactories.clear();
    for (auto & p : dfs)
        app->addDrawer(p.first, std::unique_ptr<Drawer>(p.second->createDrawer()));
}

void init(void) {
    if (showGui) {
        allow_free = false;
        displayThread = std::thread([&](void) { // new thread for opengl display
            context_lock.lock();
            app = std::make_unique<ThreedbgApp>();
            app->show();
            app->unbindContext();
            context_lock.unlock();
            while (!app->shouldClose()) {
                cache_lock.lock();
                context_lock.lock();
                app->bindContext();
                flushDrawers();
                cache_lock.unlock();
                app->loopOnce();
                app->unbindContext();
                context_lock.unlock();
                // limit fps
                static std::chrono::time_point<std::chrono::high_resolution_clock> prev_tp;
                int fps_limit = 60;
                std::this_thread::sleep_until(prev_tp + std::chrono::nanoseconds(1000000000 / fps_limit));
                prev_tp = std::chrono::high_resolution_clock::now();
            }
            app->close();
            while (!allow_free) std::this_thread::yield();
            app->bindContext();
            app.reset(nullptr);
        });
        while (!app) std::this_thread::yield();
    } else {
        app = std::make_unique<ThreedbgApp>();
        app->hide();
        app->unbindContext();
    }
}
void free(bool force) {
    context_lock.lock();
    if (force) app->close();
    context_lock.unlock();
    allow_free = true;
    if (showGui) displayThread.join();
    else {
        app->bindContext();
        app.reset(nullptr);
    }
}
void addDrawerFactory(std::string name, std::unique_ptr<DrawerFactory> && df) {
    cache_lock.lock();
    drawerFactories[name] = std::move(df);
    cache_lock.unlock();
}
bool working(void) {
    if (showGui) app->barrier();
    context_lock.lock();
    bool r = !app->shouldClose();
    context_lock.unlock();
    return r;
}
void snapshot(int & w, int & h, std::vector<unsigned char> & pixels) {
    cache_lock.lock();
    context_lock.lock();
    app->bindContext();
    flushDrawers();
    app->snapshot(w, h, pixels);
    app->unbindContext();
    context_lock.unlock();
    cache_lock.unlock();
}
Camera & camera() {
    return app->cam;
}
std::vector<std::string> getInvisible() {
    return app->getInvisible();
}
void setInvisible(std::vector<std::string> tl) {
    app->setInvisible(std::move(tl));
}
}