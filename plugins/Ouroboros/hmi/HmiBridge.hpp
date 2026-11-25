// Labels and LEDs for the controls the MOD HMI binds to our ports (lv2-hmi.h).
// mod-host takes a mutex and writes the serial queue on every call, so update()
// (audio thread) sends a port's calls only when its view changed or the port was
// just addressed. addressed()/unaddressed() run on a host thread: atomics only.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "../dsp/Looper.hpp"
#include "lv2-hmi.h"

namespace ouro {

enum HmiPort : uint8_t { kHmiRec, kHmiStop, kHmiOnOff, kHmiVolume, kHmiRedo, kHmiPortCount };

struct HmiView { TrackState state; NextAction next; bool canRedo; int selected; bool on; Transport transport; bool armed; bool waiting = false; };   // waiting: the user's press, arm or restart waits for the bar line

class HmiBridge {
public:
    void setFeature(const LV2_HMI_WidgetControl* wc) { wc_ = wc; }

    void addressed(HmiPort p, LV2_HMI_Addressing a, uint32_t caps)
    {
        if (p >= kHmiPortCount) return;
        Slot& s = slot_[p];
        s.caps.store(caps, std::memory_order_relaxed);
        s.handle.store(a, std::memory_order_relaxed);
        s.dirty.store(true, std::memory_order_release);   // published last
    }

    void unaddressed(HmiPort p)
    {
        if (p < kHmiPortCount) slot_[p].handle.store(nullptr, std::memory_order_release);
    }

    void update(const HmiView& v)
    {
        if (wc_ == nullptr) return;
        for (int i = 0; i < kHmiPortCount; ++i)
        {
            Slot& s = slot_[i];
            const bool dirty = s.dirty.exchange(false, std::memory_order_acquire);
            const LV2_HMI_Addressing a = s.handle.load(std::memory_order_acquire);
            if (dirty) sent_[i] = -1;                      // re-send everything
            if (a == nullptr) continue;
            const uint32_t caps = s.caps.load(std::memory_order_relaxed);
            const int key = fingerprint(HmiPort(i), v);
            if (key == sent_[i]) continue;
            sent_[i] = key;
            send(HmiPort(i), a, caps, v);
        }
    }

private:
    struct Slot {
        std::atomic<LV2_HMI_Addressing> handle{nullptr};
        std::atomic<uint32_t> caps{0};
        std::atomic<bool> dirty{false};
    };

    static int fingerprint(HmiPort p, const HmiView& v)
    {
        switch (p) {
        case kHmiRec:    return ((int(v.state) * 3 + int(v.next)) * 6 + int(v.transport) * 2 + v.armed) * 2 + v.waiting;
        case kHmiStop:   return (v.selected * 4 + (v.state == TrackState::Empty ? 3 : int(v.transport))) * 2 + v.on;
        case kHmiRedo:   return v.canRedo;
        case kHmiOnOff:  return v.selected * 2 + v.on;
        default:         return v.selected;
        }
    }

    void send(HmiPort p, LV2_HMI_Addressing a, uint32_t caps, const HmiView& v) const
    {
        char text[24];
        const char* label = text;
        switch (p) {
        case kHmiRec:
            label = v.armed ? "ARM" : v.transport != Transport::Running ? "PLAY"
                : v.next == NextAction::Rec ? "REC" : v.next == NextAction::Play ? "PLAY" : "DUB";
            if (caps & LV2_HMI_AddressingCapability_LED)
            {
                const bool blink = v.state == TrackState::Recording || v.state == TrackState::Overdubbing;
                const LV2_HMI_LED_Colour c = v.state == TrackState::Empty ? LV2_HMI_LED_Colour_White
                    : v.state == TrackState::Recording ? LV2_HMI_LED_Colour_Red : LV2_HMI_LED_Colour_Green;
                int t = blink ? LV2_HMI_LED_Blink_Mid : LV2_HMI_LED_Blink_None;
                LV2_HMI_LED_Colour col = c;
                if (v.waiting) { col = v.next == NextAction::Rec && v.transport == Transport::Running ? LV2_HMI_LED_Colour_Red : LV2_HMI_LED_Colour_Green; t = LV2_HMI_LED_Blink_Fast; }
                else if (v.armed) { col = LV2_HMI_LED_Colour_Red; t = LV2_HMI_LED_Blink_Slow; }
                else if (v.transport == Transport::Stopped) { col = LV2_HMI_LED_Colour_Off; t = LV2_HMI_LED_Blink_None; }
                wc_->set_led_with_blink(wc_->handle, a, col, t, t);
            }
            break;
        case kHmiStop:   // empty: a tap does nothing; stopped: a tap starts; else it toggles the track
            if (v.state == TrackState::Empty) label = "-";
            else if (v.transport == Transport::Stopped) std::snprintf(text, sizeof text, "PLAY %d", v.selected + 1);
            else std::snprintf(text, sizeof text, "%s %d", v.on ? "ON" : "OFF", v.selected + 1);
            if (caps & LV2_HMI_AddressingCapability_LED)   // green: playing, the track on (blinking: a stop finishing);
            {                                              // red: stopped; off: the track off, or empty
                LV2_HMI_LED_Colour col = LV2_HMI_LED_Colour_Off;
                int t = LV2_HMI_LED_Blink_None;
                if (v.state != TrackState::Empty && v.transport == Transport::Stopped) col = LV2_HMI_LED_Colour_Red;
                else if (v.state != TrackState::Empty && v.on)
                {
                    col = LV2_HMI_LED_Colour_Green;
                    if (v.transport == Transport::Stopping) t = LV2_HMI_LED_Blink_Mid;
                }
                wc_->set_led_with_blink(wc_->handle, a, col, t, t);
            }
            break;
        case kHmiRedo:   label = v.canRedo ? "REDO" : "-"; break;   // ASCII: the Dwarf font has no em dash
        case kHmiOnOff:  std::snprintf(text, sizeof text, "%s %d", v.on ? "ON" : "OFF", v.selected + 1); break;
        default:         std::snprintf(text, sizeof text, "Vol %d", v.selected + 1); break;
        }
        if (caps & LV2_HMI_AddressingCapability_Label) wc_->set_label(wc_->handle, a, label);
        if (p == kHmiOnOff && (caps & LV2_HMI_AddressingCapability_Value))
            wc_->set_value(wc_->handle, a, v.on ? "ON" : "OFF");
    }

    const LV2_HMI_WidgetControl* wc_ = nullptr;
    Slot slot_[kHmiPortCount];
    int sent_[kHmiPortCount] = {-1, -1, -1, -1, -1};   // audio thread only
};

}  // namespace ouro
