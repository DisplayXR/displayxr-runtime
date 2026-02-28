// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Spatial OS app state and panel management
 */

#include "spatial_app.h"
#include "logging.h"
#include <algorithm>
#include <cmath>

static float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static void LerpLayout(PanelLayout& current, const PanelLayout& target, float t) {
    current.x = Lerp(current.x, target.x, t);
    current.y = Lerp(current.y, target.y, t);
    current.width = Lerp(current.width, target.width, t);
    current.height = Lerp(current.height, target.height, t);
}

void InitializePanels(SpatialAppState& app) {
    // Participants panel (top-left)
    Panel& participants = app.panels[0];
    participants.type = PanelType::Participants;
    participants.title = L"Participants";
    participants.defaultLayout = LAYOUT_PARTICIPANTS;
    participants.current = participants.target = participants.defaultLayout;
    participants.accentColor[0] = 0.2f; participants.accentColor[1] = 0.7f; participants.accentColor[2] = 0.4f; // green

    // Chat panel (mid-left)
    Panel& chat = app.panels[1];
    chat.type = PanelType::Chat;
    chat.title = L"Chat";
    chat.defaultLayout = LAYOUT_CHAT;
    chat.current = chat.target = chat.defaultLayout;
    chat.accentColor[0] = 0.3f; chat.accentColor[1] = 0.6f; chat.accentColor[2] = 1.0f; // blue

    // Agenda panel (bottom-left)
    Panel& agenda = app.panels[2];
    agenda.type = PanelType::Agenda;
    agenda.title = L"Agenda";
    agenda.defaultLayout = LAYOUT_AGENDA;
    agenda.current = agenda.target = agenda.defaultLayout;
    agenda.accentColor[0] = 1.0f; agenda.accentColor[1] = 0.6f; agenda.accentColor[2] = 0.2f; // orange

    // Action Items panel (bottom strip)
    Panel& actions = app.panels[3];
    actions.type = PanelType::ActionItems;
    actions.title = L"Action Items";
    actions.defaultLayout = LAYOUT_ACTION_ITEMS;
    actions.current = actions.target = actions.defaultLayout;
    actions.accentColor[0] = 0.9f; actions.accentColor[1] = 0.3f; actions.accentColor[2] = 0.5f; // pink

    // Center 3D scene (rendered via projection layer, not window-space)
    Panel& scene = app.panels[4];
    scene.type = PanelType::Scene3D;
    scene.title = L"3D Scene";
    scene.defaultLayout = LAYOUT_SCENE_3D;
    scene.current = scene.target = scene.defaultLayout;

    LOG_INFO("Panels initialized: %d panels", NUM_PANELS);
}

void HandleTabKey(SpatialAppState& app) {
    // Only cycle through the 4 window-space panels (not Scene3D)
    int selectable = NUM_PANELS - 1;  // exclude Scene3D

    if (app.selectedIndex < 0) {
        app.selectedIndex = 0;
    } else {
        app.panels[app.selectedIndex].selected = false;
        app.selectedIndex = (app.selectedIndex + 1) % selectable;
    }
    app.panels[app.selectedIndex].selected = true;
    LOG_INFO("Selected panel: %ls", app.panels[app.selectedIndex].title.c_str());
}

void HandleSpaceKey(SpatialAppState& app) {
    if (app.selectedIndex < 0) return;

    if (app.focusMode && app.focusedPanel == app.selectedIndex) {
        // Exit focus mode — restore all panels
        app.focusMode = false;
        app.focusedPanel = -1;
        for (int i = 0; i < NUM_PANELS; i++) {
            app.panels[i].target = app.panels[i].defaultLayout;
            app.panels[i].targetAlpha = 1.0f;
        }
        LOG_INFO("Focus mode OFF");
    } else {
        // Enter focus mode — enlarge selected panel, shrink others
        app.focusMode = true;
        app.focusedPanel = app.selectedIndex;

        // Focused panel: center at 60% width, 60% height
        Panel& focused = app.panels[app.selectedIndex];
        focused.target = {0.20f, 0.10f, 0.60f, 0.60f};
        focused.targetAlpha = 1.0f;
        if (focused.disparityLevel > 0) {
            focused.targetDisparity = DISPARITY_LEVELS[focused.disparityLevel] * 1.5f;  // dramatic depth
        }

        // Non-focused panels: shrink to bottom strip, dim
        int slot = 0;
        float slotWidth = 0.22f;
        for (int i = 0; i < NUM_PANELS; i++) {
            if (i == app.selectedIndex) continue;
            if (app.panels[i].type == PanelType::Scene3D) continue;

            app.panels[i].target = {
                0.02f + slot * (slotWidth + 0.02f),
                0.75f,
                slotWidth,
                0.22f
            };
            app.panels[i].targetAlpha = 0.4f;
            slot++;
        }

        LOG_INFO("Focus mode ON: %ls", focused.title.c_str());
    }
}

void HandleEscapeReset(SpatialAppState& app) {
    app.focusMode = false;
    app.focusedPanel = -1;
    for (int i = 0; i < NUM_PANELS; i++) {
        app.panels[i].target = app.panels[i].defaultLayout;
        app.panels[i].targetAlpha = 1.0f;
        app.panels[i].selected = false;
        app.panels[i].disparityLevel = 0;
        app.panels[i].targetDisparity = 0.0f;
    }
    app.selectedIndex = -1;
    LOG_INFO("Layout reset to default");
}

void HandleDisparityToggle(SpatialAppState& app) {
    if (app.selectedIndex < 0) return;
    Panel& p = app.panels[app.selectedIndex];
    if (p.type == PanelType::Scene3D) return;

    p.disparityLevel = (p.disparityLevel + 1) % NUM_DISPARITY_LEVELS;
    p.targetDisparity = DISPARITY_LEVELS[p.disparityLevel];
    LOG_INFO("Panel '%ls' disparity level %d (%.3f)",
        p.title.c_str(), p.disparityLevel, p.targetDisparity);
}

void UpdatePanelAnimations(SpatialAppState& app, float deltaTime) {
    float t = 1.0f - expf(-LERP_SPEED * deltaTime);  // exponential smoothing

    for (int i = 0; i < NUM_PANELS; i++) {
        Panel& p = app.panels[i];
        LerpLayout(p.current, p.target, t);
        p.alpha = Lerp(p.alpha, p.targetAlpha, t);
        p.disparity = Lerp(p.disparity, p.targetDisparity, t);
    }
}
