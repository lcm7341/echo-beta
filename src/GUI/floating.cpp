#include "GUI/gui.hpp"

#include <Geode/ui/OverlayManager.hpp>
#include <imgui-cocos.hpp>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>

namespace chrono = std::chrono;

using namespace geode::prelude;

namespace EchoUI {
    constexpr int TOUCH_PRIORITY = -510;

    static bool g_preview = false;

    static bool touch_overlays() {
    #ifdef GEODE_IS_MOBILE
        return true;
    #else
        return false;
    #endif
    }

    static bool in_lvl() {
        auto* play = PlayLayer::get();
        return play && !play->m_isPaused;
    }

    static double now() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    class FloatingLogo : public CCNode, public CCTouchDelegate {
    public:
        static FloatingLogo* create() {
            auto* node = new FloatingLogo();
            if (node->init()) {
                node->autorelease();
                return node;
            }
            delete node;
            return nullptr;
        }

        bool init() override {
            if (!CCNode::init())
                return false;
            setID("floating-logo"_spr);
            setAnchorPoint({ .5f, .5f });
            m_logo = CCSprite::create("logo.png"_spr);
            m_logo->setScale(38.f / m_logo->getContentWidth());
            m_logo->setCascadeOpacityEnabled(true);
            addChild(m_logo);
            m_active = now();
            return true;
        }

        void onEnter() override {
            CCNode::onEnter();
            CCTouchDispatcher::get()->addTargetedDelegate(this, TOUCH_PRIORITY, true);
        }

        void onExit() override {
            CCTouchDispatcher::get()->removeDelegate(this);
            CCNode::onExit();
        }

        void visit() override {
            refresh();
            CCNode::visit();
        }

    private:
        static constexpr float SLOP = 8.f;
        static constexpr double AWAKE = 2.5;
        static constexpr double FADE = .4;

        float size() const {
            return std::clamp(Logic::get().floating_size, 24.f, 72.f);
        }

        void refresh() {
            auto& logic = Logic::get();
            if (!PlayLayer::get())
                m_renderedIn = nullptr;
            else if (logic.render_active() || logic.render_queued)
                m_renderedIn = PlayLayer::get();
            bool rendered = PlayLayer::get() && PlayLayer::get() == m_renderedIn;
            bool away = in_lvl() && logic.floating_hide_in_lvl && !rendered;
            bool shown = g_preview || (touch_overlays() && !away);
            if (!shown && m_touching)
                m_touching = false;
            if (shown && !isVisible())
                m_active = now();
            setVisible(shown);
            if (!shown)
                return;

            if (auto* themed = GUI::get().theme_logo_texture(); themed && themed != m_logo->getTexture()) {
                m_logo->setTexture(themed);
                m_logo->setTextureRect({ { 0.f, 0.f }, themed->getContentSize() });
            }
            float side = size();
            setContentSize({ side, side });
            float press = m_touching ? .92f : 1.f;
            m_logo->setScale(side / m_logo->getContentWidth() * press);
            m_logo->setPosition({ side * .5f, side * .5f });
            if (m_touching || g_preview)
                m_active = now();
            double idle = now() - m_active - AWAKE;
            float rest = (float)std::clamp(idle / FADE, 0.0, 1.0);
            m_logo->setOpacity((GLubyte)(240.f - (240.f - 95.f) * rest));
            if (!m_touching)
                setPosition(home());
        }

        CCPoint clamp_to_window(CCPoint at) const {
            auto win = CCDirector::get()->getWinSize();
            float half = size() * .5f + 4.f;
            return { std::clamp(at.x, half, win.width - half), std::clamp(at.y, half, win.height - half) };
        }

        CCPoint home() const {
            auto win = CCDirector::get()->getWinSize();
            auto& logic = Logic::get();
            if (logic.floating_x < 0.f || logic.floating_y < 0.f)
                return clamp_to_window({ win.width - 30.f, win.height * .62f });
            return clamp_to_window({ logic.floating_x * win.width, logic.floating_y * win.height });
        }

        bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
            if (!isVisible() || g_preview)
                return false;
            if (ccpDistance(touch->getLocation(), getPosition()) > size() * .5f + 8.f)
                return false;
            m_touching = true;
            m_moved = false;
            m_start = touch->getLocation();
            m_origin = getPosition();
            m_active = now();
            return true;
        }

        void ccTouchMoved(CCTouch* touch, CCEvent*) override {
            if (!m_touching)
                return;
            auto delta = touch->getLocation() - m_start;
            if (!m_moved && delta.getLength() < SLOP)
                return;
            m_moved = true;
            setPosition(clamp_to_window(m_origin + delta));
        }

        void ccTouchEnded(CCTouch*, CCEvent*) override {
            if (!m_touching)
                return;
            m_touching = false;
            m_active = now();
            if (m_moved) {
                auto win = CCDirector::get()->getWinSize();
                Logic::get().floating_x = getPositionX() / win.width;
                Logic::get().floating_y = getPositionY() / win.height;
                Logic::get().save_settings_file();
            }
            else {
                GUI::get().open_popup();
            }
        }

        void ccTouchCancelled(CCTouch*, CCEvent*) override {
            m_touching = false;
        }

        CCSprite* m_logo = nullptr;
        CCPoint m_start;
        CCPoint m_origin;
        double m_active = 0.0;
        PlayLayer* m_renderedIn = nullptr;
        bool m_touching = false;
        bool m_moved = false;
    };

    class FrameBar : public CCNode, public CCTouchDelegate {
    public:
        static FrameBar* create() {
            auto* node = new FrameBar();
            if (node->init()) {
                node->autorelease();
                return node;
            }
            delete node;
            return nullptr;
        }

        bool init() override {
            if (!CCNode::init())
                return false;
            setID("frame-bar"_spr);
            return true;
        }

        void onEnter() override {
            CCNode::onEnter();
            CCTouchDispatcher::get()->addTargetedDelegate(this, TOUCH_PRIORITY, true);
        }

        void onExit() override {
            release(false);
            CCTouchDispatcher::get()->removeDelegate(this);
            CCNode::onExit();
        }

        void visit() override {
            refresh();
            CCNode::visit();
        }

    private:
        void refresh() {
            auto& gui = GUI::get();
            auto& imgui = ImGuiCocos::get();
            if (!imgui.isInitialized())
                return;
            auto& logic = Logic::get();
            bool menu = imgui.isVisible() && !gui.bar_only;
            if (!PlayLayer::get())
                logic.frame_bar_open = false;
            else if (logic.frame_advance)
                logic.frame_bar_open = true;
            bool shown = in_lvl() && (logic.frame_advance || logic.frame_bar_open) && touch_overlays() && !menu && !logic.render_capturing();
            if (shown && !gui.bar_only) {
                gui.bar_only = true;
                gui.bar_area = gui.bar_back = gui.bar_step = gui.bar_resume = gui.bar_best = ImVec4();
                imgui.setVisible(true);
            }
            else if (!shown && gui.bar_only) {
                release(false);
                gui.bar_only = false;
                imgui.setVisible(false);
            }
            else if (!shown) {
                release(false);
            }
        }

        static bool within(const ImVec4& box, ImVec2 at, float slack) {
            return box.z > box.x && at.x >= box.x - slack && at.y >= box.y - slack && at.x <= box.z + slack && at.y <= box.w + slack;
        }

        bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
            auto& gui = GUI::get();
            if (!gui.bar_only || m_down)
                return false;
            ImVec2 at = ImGuiCocos::cocosToFrame(touch->getLocation());
            if (!within(gui.bar_area, at, 0.f))
                return false;
            float slack = 6.f;
            auto& logic = Logic::get();
            auto now = chrono::steady_clock::now();
            m_down = true;
            gui.bar_pressed = 0;
            if (within(gui.bar_back, at, slack)) {
                gui.bar_pressed = 1;
                logic.holding_back = true;
                logic.back_hold_start = now;
                logic.last_back_step = now;
                logic.back_tap_queued = true;
            }
            else if (within(gui.bar_step, at, slack)) {
                gui.bar_pressed = 2;
                logic.holding_advance = true;
                logic.advance_hold_start = now;
                logic.last_advance_step = now;
                logic.advance_one_frame();
            }
            else if (within(gui.bar_resume, at, slack)) {
                gui.bar_pressed = 3;
            }
            else if (within(gui.bar_best, at, slack)) {
                gui.bar_pressed = 4;
            }
            else {
                m_dragging = true;
                m_dragged = false;
                m_start = at;
                m_origin = ImVec2((gui.bar_area.x + gui.bar_area.z) * .5f, gui.bar_area.y);
            }
            return true;
        }

        void ccTouchMoved(CCTouch* touch, CCEvent*) override {
            if (!m_dragging)
                return;
            ImVec2 at = ImGuiCocos::cocosToFrame(touch->getLocation());
            ImVec2 delta(at.x - m_start.x, at.y - m_start.y);
            if (!m_dragged && delta.x * delta.x + delta.y * delta.y < 64.f)
                return;
            m_dragged = true;
            ImVec2 display = ImGui::GetIO().DisplaySize;
            if (display.x < 1.f || display.y < 1.f)
                return;
            auto& logic = Logic::get();
            logic.frame_bar_x = std::clamp((m_origin.x + delta.x) / display.x, 0.f, 1.f);
            logic.frame_bar_y = std::clamp((m_origin.y + delta.y) / display.y, 0.f, 1.f);
        }

        void ccTouchEnded(CCTouch* touch, CCEvent*) override {
            auto& gui = GUI::get();
            const auto& box = gui.bar_pressed == 4 ? gui.bar_best : gui.bar_resume;
            release(within(box, ImGuiCocos::cocosToFrame(touch->getLocation()), 6.f));
        }

        void ccTouchCancelled(CCTouch*, CCEvent*) override {
            release(false);
        }

        void release(bool onResume) {
            if (!m_down)
                return;
            m_down = false;
            if (m_dragging) {
                m_dragging = false;
                if (m_dragged)
                    Logic::get().save_settings_file();
            }
            auto& gui = GUI::get();
            auto& logic = Logic::get();
            int which = gui.bar_pressed;
            gui.bar_pressed = 0;
            if (which == 1)
                logic.holding_back = false;
            if (which == 2) {
                logic.holding_advance = false;
                logic.disarm_length_pause_if_holding();
            }
            if (which == 3 && onResume)
                gui.set_frame_advance(!logic.frame_advance);
            if (which == 4 && onResume)
                logic.request_best_frame();
        }

        bool m_down = false;
        bool m_dragging = false, m_dragged = false;
        ImVec2 m_start, m_origin;
    };

#ifdef GEODE_IS_MOBILE
    class MenuDismiss : public CCNode, public CCTouchDelegate {
    public:
        static MenuDismiss* create() {
            auto* node = new MenuDismiss();
            if (node->init()) {
                node->autorelease();
                return node;
            }
            delete node;
            return nullptr;
        }

        void onEnter() override {
            CCNode::onEnter();
            CCTouchDispatcher::get()->addTargetedDelegate(this, TOUCH_PRIORITY + 1, true);
        }

        void onExit() override {
            CCTouchDispatcher::get()->removeDelegate(this);
            CCNode::onExit();
        }

    private:
        bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
            auto& imgui = ImGuiCocos::get();
            auto& gui = GUI::get();
            if (!imgui.isInitialized() || !imgui.isVisible() || gui.bar_only)
                return false;
            auto* context = ImGui::GetCurrentContext();
            if (!context)
                return false;
            ImVec2 at = ImGuiCocos::cocosToFrame(touch->getLocation());
            for (ImGuiWindow* window : context->Windows) {
                if (!window->Active || window->Hidden || (window->Flags & ImGuiWindowFlags_ChildWindow))
                    continue;
                if (!window->Rect().Contains(at))
                    continue;
                auto& io = ImGui::GetIO();
                io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
                io.AddMousePosEvent(at.x, at.y);
                io.AddMouseButtonEvent(0, true);
                return true;
            }
            if (gui.closing)
                return false;
            gui.begin_close();
            return true;
        }
    };
#endif

    void preview_logo(bool on) {
        g_preview = on;
    }
}

$on_mod(Loaded) {
    queueInMainThread([] {
        auto* overlay = OverlayManager::get();
        if (auto* bar = EchoUI::FrameBar::create())
            overlay->addChild(bar);
        if (auto* logo = EchoUI::FloatingLogo::create())
            overlay->addChild(logo);
#ifdef GEODE_IS_MOBILE
        if (auto* dismiss = EchoUI::MenuDismiss::create())
            overlay->addChild(dismiss);
#endif
    });
}
