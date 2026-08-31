<script>
  import { colors } from "./colors.js";
  import Controller from "./Controller.svelte";
  import ControllerLandscape from "./ControllerLandscape.svelte";
  import ActionIcon from "./ActionIcon.svelte";
  import SkillsView from "./SkillsView.svelte";
  import UploadView from "./UploadView.svelte";
  import Calibration from "./Calibration.svelte";
  import WiFiSetup from "./WiFiSetup.svelte";
  import APModeConfig from "./APModeConfig.svelte";

  let { network } = $props();
  import ActionAssignment from "./ActionAssignment.svelte";
  import AddActionView from "./AddActionView.svelte";
  import ChatView from "./chat/ChatView.svelte";
  import SkillsMarketplace from "./SkillsMarketplace.svelte";
  import ServoStudio from "./ServoStudio.svelte";

  const YELLOW = colors.mpx.primary;

  let activeTab = $state("home");
  let useLandscape = $state(false);
  let panelEl = $state(null);

  const INNER_RATIO = 881 / 457; // landscape inner aspect ratio

  function setInnerSizing() {
    if (!panelEl) return;
    const { width, height } = panelEl.getBoundingClientRect();
    const panelRatio = width / height;
    if (panelRatio > INNER_RATIO) {
      panelEl.style.setProperty('--inner-w', 'auto');
      panelEl.style.setProperty('--inner-h', '100%');
    } else {
      panelEl.style.setProperty('--inner-w', '100%');
      panelEl.style.setProperty('--inner-h', 'auto');
    }
  }

  $effect(() => {
    if (!panelEl) return;
    const ro = new ResizeObserver(([entry]) => {
      const { width, height } = entry.contentRect;
      useLandscape = width >= height * 1.3;
      setInnerSizing();
    });
    ro.observe(panelEl);
    return () => ro.disconnect();
  });

  // Re-run sizing immediately whenever the controller switches
  $effect(() => {
    if (!panelEl) return;
    useLandscape; // track changes
    setInnerSizing();
  });
  let isFullscreen = $state(false);
  let isEditMode = $state(false);
  let showActionAssignment = $state(false);
  let actionAssignments = $state({});

  // Lock body scroll when fullscreen
  $effect(() => {
    if (isFullscreen) {
      document.body.style.overflow = 'hidden';
    } else {
      document.body.style.overflow = '';
    }
    return () => { document.body.style.overflow = ''; };
  });

  let activeView = $state("main"); // "main" | "upload"

  // ── Connection mode detection ──────────────────────────────
  /** "ap" if the page is loaded via 192.168.2.1, otherwise "sta". */
  let connectionMode = $derived(
    typeof window !== "undefined" && window.location.hostname === "192.168.2.1"
      ? "ap"
      : "sta"
  );

  let showAllActions = $state(false);
  let showAddAction = $state(false);
  let customActions = $state([]);
  let pinnedActions = $state([]);
  let showGaitSection = $state(false);
  let longPressTarget = $state(null);
  let showAllAdjust = $state(false);
  let selectedAdjust = $state(0);
  let showCalibration = $state(false);
  let showStudio = $state(false);
  let showWiFi = $state(false);
  let showAPConfig = $state(false);
  let adjustDraggingId = $state(null);
  let adjustDragBarEl = $state(null);

  let adjustProps = $state([
    // Each entry has a `key` mapping to the /v1/robot/config API parameter name
    { label: 'Period', key: 'period',    value: 80,  min: 0,   max: 200, unit: 'ms',  isCenter: false },
    { label: 'Height', key: 'height',    value: 70,  min: 0,   max: 150, unit: 'mm',  isCenter: false },
    { label: 'Lift',   key: 'up_height', value: 10,  min: 0,   max: 50,  unit: 'mm',  isCenter: false },
    { label: 'Stride', key: 'stride',    value: 10,  min: 0,   max: 50,  unit: 'mm',  isCenter: false },
    { label: 'Tilt',   key: 'tilt',      value: 0,   min: -30, max: 30,  unit: '°',   isCenter: true },
  ]);

  const adjustPerPage = 3;
  function adjustTotalPages() {
    return Math.ceil(adjustProps.length / adjustPerPage);
  }

  // ── Robot config API ────────────────────────────────────────
  let configLoading = $state(false);

  async function fetchConfig() {
    configLoading = true;
    try {
      const res = await fetch("/v1/robot/status");
      if (res.ok) {
        const data = await res.json();
        const cfg = data.config || {};
        for (const prop of adjustProps) {
          if (cfg[prop.key] !== undefined) {
            prop.value = cfg[prop.key];
          }
        }
      }
    } catch { /* robot may be unreachable — keep defaults */ }
    configLoading = false;
  }

  async function saveConfig() {
    const body = {};
    for (const prop of adjustProps) {
      body[prop.key] = prop.value;
    }
    try {
      await fetch("/v1/robot/config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
    } catch { /* ignore transient errors */ }
  }

  // ── localStorage persistence ──────────────────────────────
  const STORAGE_KEY_ASSIGNMENTS = "mpx_action_assignments";
  const STORAGE_KEY_CUSTOM = "mpx_custom_actions";
  const STORAGE_KEY_PINNED = "mpx_pinned_actions";

  // Load persisted state on mount
  $effect(() => {
    try {
      const saved = localStorage.getItem(STORAGE_KEY_ASSIGNMENTS);
      if (saved) actionAssignments = JSON.parse(saved);
    } catch {}
    try {
      const saved = localStorage.getItem(STORAGE_KEY_CUSTOM);
      if (saved) customActions = JSON.parse(saved);
    } catch {}
    try {
      const saved = localStorage.getItem(STORAGE_KEY_PINNED);
      if (saved) pinnedActions = JSON.parse(saved);
    } catch {}
  });

  // Persist on change
  $effect(() => {
    if (typeof window === "undefined") return;
    // track changes
    const a = JSON.stringify(actionAssignments);
    const c = JSON.stringify(customActions);
    const p = JSON.stringify(pinnedActions);
    try { localStorage.setItem(STORAGE_KEY_ASSIGNMENTS, a); } catch {}
    try { localStorage.setItem(STORAGE_KEY_CUSTOM, c); } catch {}
    try { localStorage.setItem(STORAGE_KEY_PINNED, p); } catch {}
  });

  $effect(() => {
    fetchConfig();
  });

  const actions = [
    // ── Movement gaits ──
    { id: "advance",  label: "Forward",      emoji: "🚶", color: colors.mpx.action_green, category: "gait" },
    { id: "back",     label: "Backward",     emoji: "🔙", color: colors.mpx.action_green, category: "gait" },
    { id: "left",     label: "Strafe Left",  emoji: "◀️",  color: colors.mpx.action_green, category: "gait" },
    { id: "right",    label: "Strafe Right", emoji: "▶️",  color: colors.mpx.action_green, category: "gait" },
    { id: "turnL",    label: "Turn Left",    emoji: "↺",  color: "#0E8C8C",              category: "gait" },
    { id: "turnR",    label: "Turn Right",   emoji: "↻",  color: "#0E8C8C",              category: "gait" },
    { id: "stanford", label: "Trot",         emoji: "🐕", color: "#14A37F",              category: "gait" },
    { id: "step",     label: "Step",         emoji: "🦶", color: "#14A37F",              category: "gait" },
    { id: "testspeed",label: "Speed Test",   emoji: "⚡",  color: "#D14949",              category: "gait" },
    { id: "jump",     label: "Jump",         emoji: "🤸", color: "#D97A29",              category: "gait" },
    { id: "jumpfwd",  label: "Jump Fwd",     emoji: "🏃", color: "#C4901A",              category: "gait" },
    // ── Leg lifts ──
    { id: "flegR",    label: "Lift FR",      emoji: "🦵", color: "#5FAD41",              category: "gait" },
    { id: "flegL",    label: "Lift FL",      emoji: "🦵", color: "#5FAD41",              category: "gait" },
    { id: "blegR",    label: "Lift RR",      emoji: "🦵", color: "#5FAD41",              category: "gait" },
    { id: "blegL",    label: "Lift RL",      emoji: "🦵", color: "#5FAD41",              category: "gait" },
    // ── Height ──
    { id: "heightup",   label: "Height Up",   emoji: "⬆️",  color: "#D96098",              category: "gait" },
    { id: "heightdown", label: "Height Down", emoji: "⬇️",  color: "#D96098",              category: "gait" },
    // ── Diagonal moves ──
    { id: "moveLF", label: "Diag FL", emoji: "↗️",  color: "#4A8BC2",              category: "gait" },
    { id: "moveRF", label: "Diag FR", emoji: "↖️",  color: "#4A8BC2",              category: "gait" },
    { id: "moveLB", label: "Diag BL", emoji: "↘️",  color: "#4A8BC2",              category: "gait" },
    { id: "moveRB", label: "Diag BR", emoji: "↙️",  color: "#4A8BC2",              category: "gait" },
    // ── Skills (all built-in actions are gaits) ──
    { id: "sit",      label: "Sit",          emoji: "🪑", color: colors.mpx.action_blue,   category: "gait" },
    { id: "stretch",  label: "Stretch",      emoji: "🧘", color: "#D96098",               category: "gait" },
    { id: "twerk",    label: "Twerk",        emoji: "💃", color: colors.mpx.action_purple, category: "gait" },
    { id: "roll",     label: "Roll",         emoji: "🔄", color: "#14A37F",               category: "gait" },
    { id: "pitch",    label: "Pitch",        emoji: "📐", color: "#14A37F",               category: "gait" },
    { id: "balance",  label: "Balance",      emoji: "⚖️",  color: "#14A37F",               category: "gait" },
    { id: "init",     label: "Init",         emoji: "🏁", color: "#7C7F7C",               category: "gait" },
    { id: "none",     label: "Stop",         emoji: "⏹️",  color: "#D14949",               category: "gait" },
    { id: "frontkick",label: "Front Kick",   emoji: "🦶", color: "#C44569",               category: "gait" },
    { id: "wiggle",   label: "Wiggle",       emoji: "🐕", color: "#C44569",               category: "gait" },
    { id: "wiggleL",  label: "Wiggle ◀",     emoji: "◀️",  color: "#C44569",               category: "gait" },
    { id: "wiggleR",  label: "Wiggle ▶",     emoji: "▶️",  color: "#C44569",               category: "gait" },
    { id: "buttshrug",  label: "Butt Shrug",   emoji: "🍑", color: "#C44569",             category: "gait" },
    { id: "buttshrugL", label: "Shrug ◀",      emoji: "◀️",  color: "#C44569",             category: "gait" },
    { id: "buttshrugR", label: "Shrug ▶",      emoji: "▶️",  color: "#C44569",             category: "gait" },
    { id: "bowback",  label: "Bow",          emoji: "🙇", color: "#AD5FBF",               category: "gait" },
    { id: "bodycycle",label: "Body Circle",  emoji: "🔄", color: "#AD5FBF",               category: "gait" },
    { id: "headellipse",label: "Head Circle", emoji: "🔄", color: "#AD5FBF",              category: "gait" },
  ];

  // The second tab used to be the raw file browser, which is where every
  // skill pushed with `mpx-cli deploy` ended up looking like a stray blob.
  // It is now the Skills screen; the file browser lives inside it as a
  // segment, so nothing was lost.
  /* The store is a destination, not a setting.
   *
   * It used to be a row inside Settings — three taps deep, next to Wi-Fi and
   * calibration, which is where you put a configuration screen and not where
   * anyone looks to browse. Somewhere you go to spend money belongs in the
   * navigation. Placed after Skills because the pair reads in the order you
   * use them: find something, then run it. */
  const tabs = [
    { id: "home",     label: "Home",     icon: "home" },
    { id: "skills",   label: "Skills",   icon: "skills" },
    { id: "store",    label: "Store",    icon: "store" },
    { id: "chat",     label: "Chat",     icon: "chat" },
    { id: "settings", label: "Settings", icon: "settings" },
  ];

  // ── Adjust slider interaction ──
  function adjustPct(prop) {
    return ((prop.value - prop.min) / (prop.max - prop.min)) * 100;
  }
  function adjustNeutralPct(prop) {
    if (prop.isCenter) {
      return ((0 - prop.min) / (prop.max - prop.min)) * 100;
    }
    return 0;
  }
  function adjustSliderValueFromPointer(prop, clientX) {
    if (!adjustDragBarEl) return prop.value;
    const rect = adjustDragBarEl.getBoundingClientRect();
    const pct = (clientX - rect.left) / rect.width;
    return Math.round(prop.min + pct * (prop.max - prop.min));
  }
  function adjustStartDrag(e, propIdx) {
    const prop = adjustProps[propIdx];
    if (!prop) return;
    const handle = e.currentTarget;
    handle.setPointerCapture(e.pointerId);
    adjustDragBarEl = handle.parentElement;
    adjustDraggingId = propIdx;
    const val = adjustSliderValueFromPointer(prop, e.clientX);
    adjustProps[propIdx].value = Math.max(prop.min, Math.min(prop.max, val));
  }
  function adjustOnDragMove(e) {
    if (adjustDraggingId === null) return;
    const prop = adjustProps[adjustDraggingId];
    if (!prop) return;
    const val = adjustSliderValueFromPointer(prop, e.clientX);
    adjustProps[adjustDraggingId].value = Math.max(prop.min, Math.min(prop.max, val));
  }
  function adjustEndDrag() {
    adjustDraggingId = null;
    adjustDragBarEl = null;
    saveConfig();
  }
  function adjustStep(propIdx, delta) {
    const prop = adjustProps[propIdx];
    if (!prop) return;
    adjustProps[propIdx].value = Math.max(prop.min, Math.min(prop.max, prop.value + delta));
    saveConfig();
  }
  function adjustCurrentPage() {
    return Math.floor(selectedAdjust / adjustPerPage);
  }

  const actionsPerPage = 8;
  function totalPages() {
    return Math.ceil((actions.length + customActions.length + 1) / actionsPerPage);
  }

  let allActions = $derived([...actions, ...customActions.map(c => ({ ...c, category: c.category || "custom" }))]);

  let gaitActions = $derived(allActions.filter(a => a.category === "gait"));
  let skillActions = $derived(allActions.filter(a => a.category !== "gait"));

  function handleAddAction(newAction) {
    customActions = [...customActions, newAction];
  }

  async function sendGait(mode) {
    try {
      await fetch("/v1/robot/gait", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ mode }),
      });
    } catch { /* ignore */ }
  }

  function togglePin(actionId) {
    if (pinnedActions.includes(actionId)) {
      pinnedActions = pinnedActions.filter(id => id !== actionId);
    } else {
      pinnedActions = [...pinnedActions, actionId];
    }
  }

  function isPinned(actionId) {
    return pinnedActions.includes(actionId);
  }

  // Get pinned action objects
  let pinnedActionObjects = $derived(
    pinnedActions.map(id => allActions.find(a => a.id === id)).filter(Boolean)
  );

  const pinnedPerPage = 8;
  let pinnedTotalPages = $derived(Math.max(1, Math.ceil(pinnedActionObjects.length / pinnedPerPage)));

  // ── Fullscreen API ──────────────────────────────────────
  async function toggleFullscreen() {
    if (isFullscreen) {
      // Exit fullscreen
      isFullscreen = false;
      try {
        if (document.fullscreenElement) {
          await document.exitFullscreen();
        }
      } catch {}
    } else {
      // Enter fullscreen using the Fullscreen API
      isFullscreen = true;
      try {
        await document.documentElement.requestFullscreen();
      } catch {
        // Fallback: just use CSS fullscreen if API fails
      }
    }
  }

  // Listen for fullscreen exit via Esc
  $effect(() => {
    function onFsChange() {
      if (!document.fullscreenElement && isFullscreen) {
        isFullscreen = false;
      }
    }
    document.addEventListener('fullscreenchange', onFsChange);
    return () => document.removeEventListener('fullscreenchange', onFsChange);
  });

  function selectTab(id) {
    activeTab = id;
  }
</script>

<div class="home-root">
  <!-- ═══ Top Header ═══ -->
  <header class="top-header" style="background: {YELLOW}">
    <div class="header-left">
      <!-- Connection mode indicator -->
      <div class="conn-mode-pill" class:mode-ap={connectionMode === "ap"} class:mode-sta={connectionMode === "sta"}>
        <span class="conn-mode-dot"></span>
        <span class="conn-mode-label">{connectionMode === "ap" ? "AP" : "STA"}</span>
      </div>
    </div>

    <div class="header-center">
      <img class="header-logo" src="/md.svg" alt="MangDang" />
    </div>

    <div class="header-right">
    </div>
  </header>

  <!-- ═══ Content Area ═══ -->
  <main class="content">
    {#if activeTab === "home" && showAddAction}
      <AddActionView
        onSave={handleAddAction}
        onBack={() => { showAddAction = false; }}
      />
    {:else if activeTab === "home" && showActionAssignment}
      <!-- ═══ Action Assignment Sub-screen ═══ -->
      <ActionAssignment
        assignments={actionAssignments}
        customActions={customActions}
        onAddAction={() => { showAddAction = true; }}
        onDeleteAction={(actionId) => {
          customActions = customActions.filter(c => c.id !== actionId);
          pinnedActions = pinnedActions.filter(id => id !== actionId);
        }}
        onSave={(updated) => {
          actionAssignments = updated;
        }}
        onBack={() => {
          showActionAssignment = false;
          isEditMode = false;
        }}
      />
    {:else if activeTab === "home" && showAllActions}
      <!-- ═══ Full-page All Actions View ═══ -->
      <div class="all-actions-page" style="--yellow: {YELLOW}">
        <header class="all-actions-header">
          <button class="back-btn" aria-label="Back" onclick={() => showAllActions = false}>
            <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
              <line x1="19" y1="12" x2="5" y2="12"/>
              <polyline points="12 19 5 12 12 5"/>
            </svg>
          </button>
          <h2 class="all-actions-title">All Actions</h2>
          <div style="width:32px"></div>
        </header>
        <div class="all-actions-scroll">
          <!-- ═══ Gait Section (collapsible) ═══ -->
          {#if gaitActions.length > 0}
            <div class="all-actions-category">
              <button class="all-actions-cat-header" onclick={() => showGaitSection = !showGaitSection} aria-label="Toggle gait section">
                <h3 class="all-actions-cat-title">🚶 Gait</h3>
                <svg class="collapse-chevron" class:collapsed={!showGaitSection} width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                  <polyline points="6 9 12 15 18 9"/>
                </svg>
              </button>
              {#if showGaitSection}
              <div class="actions-full-grid">
                {#each gaitActions as action}
                  <div class="action-grid-item all-action-item" style="--action-color: {action.color}" role="button" tabindex="0">
                    <div class="action-grid-icon">
                      <ActionIcon id={action.id} emoji={action.emoji} />
                    </div>
                    <span class="action-grid-label">{action.label}</span>
                    <!-- Pin button -->
                    <button
                      class="pin-trigger-btn"
                      class:pinned={isPinned(action.id)}
                      onclick={(e) => { e.stopPropagation(); togglePin(action.id); }}
                      aria-label={isPinned(action.id) ? 'Unpin {action.label}' : 'Pin {action.label}'}
                    >
                      <svg width="12" height="12" viewBox="0 0 24 24" fill={isPinned(action.id) ? 'currentColor' : 'none'} stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                        <line x1="12" y1="17" x2="12" y2="22"/>
                        <path d="M5 17h14v-2.5c0-.83-.67-1.5-1.5-1.5h-11c-.83 0-1.5.67-1.5 1.5V17z"/>
                        <path d="M12 14V7l-2.33-4.67h4.66L12 7z"/>
                      </svg>
                    </button>
                  </div>
                {/each}
              </div>
              {/if}
            </div>
          {/if}

          <!-- ═══ Skill Section ═══ -->
          {#if skillActions.length > 0}
            <div class="all-actions-category">
              <h3 class="all-actions-cat-title">🎯 Skill</h3>
              <div class="actions-full-grid">
                {#each skillActions as action}
                  <div class="action-grid-item all-action-item" style="--action-color: {action.color}" role="button" tabindex="0">
                    <div class="action-grid-icon">
                      <ActionIcon id={action.id} emoji={action.emoji} />
                    </div>
                    <span class="action-grid-label">{action.label}</span>
                    <!-- Pin button -->
                    <button
                      class="pin-trigger-btn"
                      class:pinned={isPinned(action.id)}
                      onclick={(e) => { e.stopPropagation(); togglePin(action.id); }}
                      aria-label={isPinned(action.id) ? 'Unpin {action.label}' : 'Pin {action.label}'}
                    >
                      <svg width="12" height="12" viewBox="0 0 24 24" fill={isPinned(action.id) ? 'currentColor' : 'none'} stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                        <line x1="12" y1="17" x2="12" y2="22"/>
                        <path d="M5 17h14v-2.5c0-.83-.67-1.5-1.5-1.5h-11c-.83 0-1.5.67-1.5 1.5V17z"/>
                        <path d="M12 14V7l-2.33-4.67h4.66L12 7z"/>
                      </svg>
                    </button>
                    {#if action.category === "skill" || action.category === "custom"}
                    <!-- Delete custom action -->
                    <button
                      class="delete-action-btn"
                      onclick={(e) => {
                        e.stopPropagation();
                        customActions = customActions.filter(c => c.id !== action.id);
                        pinnedActions = pinnedActions.filter(id => id !== action.id);
                      }}
                      aria-label="Delete {action.label}"
                    >
                      <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="white" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                        <polyline points="3 6 5 6 21 6"/>
                        <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/>
                      </svg>
                    </button>
                    {/if}
                  </div>
                {/each}
              </div>
            </div>
          {/if}

          <!-- Add button -->
          <div class="all-actions-add-wrap">
            <button class="action-grid-item add-item" onclick={() => { showAllActions = false; showAddAction = true; }}>
              <div class="action-grid-icon add-icon-box">
                <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                  <line x1="12" y1="5" x2="12" y2="19"/>
                  <line x1="5" y1="12" x2="19" y2="12"/>
                </svg>
              </div>
              <span class="action-grid-label">Add</span>
            </button>
          </div>
        </div>
      </div>
    {:else if activeTab === "home" && showAllAdjust}
      <!-- ═══ Full-page All Adjust View ═══ -->
      <div class="all-adjust-page" style="--yellow: {YELLOW}">
        <header class="all-adjust-header">
          <button class="back-btn" aria-label="Back" onclick={() => showAllAdjust = false}>
            <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
              <line x1="19" y1="12" x2="5" y2="12"/>
              <polyline points="12 19 5 12 12 5"/>
            </svg>
          </button>
          <h2 class="all-adjust-title">All Adjust</h2>
          <div style="width:32px"></div>
        </header>
        <div class="all-adjust-list">
          {#each adjustProps as prop, idx}
            <div class="all-adjust-item">
              <div class="all-adjust-item-header">
                <span class="all-adjust-item-label">{prop.label}</span>
                <span class="all-adjust-item-value">{prop.value}{prop.unit}</span>
              </div>
              <div class="all-adjust-slider-row">
                <button class="adjust-step-btn" aria-label="Decrease {prop.label}" onclick={() => adjustStep(idx, -1)}>
                  <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                    <line x1="5" y1="12" x2="19" y2="12"/>
                  </svg>
                </button>
                <div class="all-adjust-slider-container">
                  <div class="all-adjust-slider-bar">
                    <div
                      class="all-adjust-slider-fill"
                      style="width: {adjustPct(prop)}%"
                    ></div>
                    <div class="all-adjust-slider-handle current" style="left: {adjustPct(prop)}%"
                      role="slider" tabindex="0"
                      aria-label={prop.label}
                      aria-valuemin={prop.min}
                      aria-valuemax={prop.max}
                      aria-valuenow={prop.value}
                      onpointerdown={(e) => adjustStartDrag(e, idx)}
                      onpointermove={adjustOnDragMove}
                      onpointerup={adjustEndDrag}
                      onpointercancel={adjustEndDrag}
                    ></div>
                  </div>
                </div>
                <button class="adjust-step-btn" aria-label="Increase {prop.label}" onclick={() => adjustStep(idx, 1)}>
                  <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                    <line x1="12" y1="5" x2="12" y2="19"/>
                    <line x1="5" y1="12" x2="19" y2="12"/>
                  </svg>
                </button>
              </div>
            </div>
          {/each}
        </div>
      </div>
    {:else if activeTab === "home"}
      <div class="home-content" style="--thumb-color: {YELLOW}; --yellow: {YELLOW}">
        <!-- ═══ Controller Panel ═══ -->
        <div
          class="controller-panel"
          class:landscape={useLandscape}
          class:fullscreen={isFullscreen}
          style="background: {useLandscape ? '#fff' : YELLOW}"
          bind:this={panelEl}
        >
          {#if useLandscape}
            <ControllerLandscape assignments={actionAssignments} />
          {:else}
            <Controller assignments={actionAssignments} />
          {/if}

          <!-- ═══ Overlay Control Buttons ═══ -->
          <div class="ctrl-overlay-btns">
            <button
              class="ctrl-overlay-btn"
              onclick={toggleFullscreen}
              aria-label={isFullscreen ? 'Minimize' : 'Fullscreen'}
              title={isFullscreen ? 'Minimize' : 'Fullscreen'}
            >
              {#if isFullscreen}
                <!-- Minimize icon -->
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                  <polyline points="4 14 10 14 10 20"/>
                  <polyline points="20 10 14 10 14 4"/>
                  <line x1="14" y1="10" x2="21" y2="3"/>
                  <line x1="3" y1="21" x2="10" y2="14"/>
                </svg>
              {:else}
                <!-- Fullscreen icon -->
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                  <polyline points="15 3 21 3 21 9"/>
                  <polyline points="9 21 3 21 3 15"/>
                  <line x1="21" y1="3" x2="14" y2="10"/>
                  <line x1="3" y1="21" x2="10" y2="14"/>
                </svg>
              {/if}
            </button>

            {#if !isFullscreen}
            <button
              class="ctrl-overlay-btn"
              class:active={isEditMode}
              onclick={() => {
                isEditMode = !isEditMode;
                if (isEditMode) showActionAssignment = true;
              }}
              aria-label="Edit actions"
              title="Edit actions"
            >
              <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"/>
                <path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z"/>
              </svg>
            </button>
            {/if}
          </div>
        </div>

        <!-- ═══ Scrollable cards below ═══ -->
        <div class="home-content-scroll">

        <!-- ═══ Actions Card (Pinned) ═══ -->
        <div class="section-card">
          <div class="section-card-header">
            <h2 class="section-card-title">Quick Actions</h2>
            <button class="see-all-btn" onclick={() => showAllActions = true}>See all</button>
          </div>
          <div class="action-grid-scroll-wrap">
            {#if pinnedActionObjects.length === 0}
              <p class="no-pinned-hint">Long-press an action in "See all" to pin it here.</p>
            {:else}
            <div class="action-grid-scroll">
              {#each { length: pinnedTotalPages } as _, pageIdx}
                  <div class="action-grid-page">
                    {#each pinnedActionObjects.slice(pageIdx * pinnedPerPage, pageIdx * pinnedPerPage + pinnedPerPage) as action}
                      <div
                        class="action-grid-item pinned-item"
                        class:show-trash={longPressTarget === action.id}
                        style="--action-color: {action.color}; position: relative;"
                        role="button"
                        tabindex="0"
                        onclick={() => sendGait(action.id)}
                        onpointerdown={(e) => {
                          const timer = setTimeout(() => {
                            longPressTarget = action.id;
                          }, 600);
                          e.currentTarget._longPressTimer = timer;
                        }}
                        onpointerup={(e) => {
                          clearTimeout(e.currentTarget._longPressTimer);
                          longPressTarget = null;
                        }}
                        onpointerleave={(e) => {
                          clearTimeout(e.currentTarget._longPressTimer);
                          longPressTarget = null;
                        }}
                        onpointercancel={(e) => {
                          clearTimeout(e.currentTarget._longPressTimer);
                          longPressTarget = null;
                        }}
                      >
                        <div class="action-grid-icon">
                          <ActionIcon id={action.id} emoji={action.emoji} />
                        </div>
                        <span class="action-grid-label">{action.label}</span>
                        <!-- Trash button (shown on long-press) -->
                        <button
                          class="unpin-trash-btn"
                          onclick={(e) => {
                            e.stopPropagation();
                            togglePin(action.id);
                          }}
                          aria-label="Remove {action.label}"
                        >
                          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="white" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                            <polyline points="3 6 5 6 21 6"/>
                            <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/>
                          </svg>
                        </button>
                      </div>
                    {/each}
                  </div>
              {/each}
            </div>
            <!-- Page dots -->
            {#if pinnedTotalPages > 1}
            <div class="page-dots">
              {#each { length: pinnedTotalPages } as _, i}
                <span class="page-dot" class:active={i === 0}></span>
              {/each}
            </div>
            {/if}
            {/if}
          </div>
        </div>

        <!-- ═══ Adjust Card (swipeable) ═══ -->
        <div class="section-card">
          <div class="section-card-header">
            <h2 class="section-card-title">Adjust</h2>
            <button class="see-all-btn" onclick={() => showAllAdjust = true}>See all</button>
          </div>

          <div class="adjust-scroll-wrap">
            <div class="adjust-scroll">
              {#each { length: adjustTotalPages() } as _, pageIdx}
                <div class="adjust-page">
                  {#each adjustProps.slice(pageIdx * adjustPerPage, pageIdx * adjustPerPage + adjustPerPage) as prop, i}
                    <button
                      class="adjust-selector-btn"
                      class:active={selectedAdjust === pageIdx * adjustPerPage + i}
                      onclick={() => selectedAdjust = pageIdx * adjustPerPage + i}
                    >
                      <div class="adjust-selector-circle" class:active={selectedAdjust === pageIdx * adjustPerPage + i} style="--sel-color: {YELLOW}">
                        {#if (pageIdx * adjustPerPage + i) === 0}
                          <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                            <circle cx="12" cy="12" r="10"/>
                            <polyline points="12 6 12 12 16 14"/>
                          </svg>
                        {:else if (pageIdx * adjustPerPage + i) === 1}
                          <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                            <line x1="12" y1="5" x2="12" y2="19"/>
                            <polyline points="9 8 12 5 15 8"/>
                            <polyline points="9 16 12 19 15 16"/>
                          </svg>
                        {:else if (pageIdx * adjustPerPage + i) === 2}
                          <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                            <line x1="12" y1="5" x2="12" y2="19"/>
                            <polyline points="9 8 12 5 15 8"/>
                          </svg>
                        {:else if (pageIdx * adjustPerPage + i) === 3}
                          <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                            <polyline points="17 8 21 12 17 16"/>
                            <polyline points="7 8 3 12 7 16"/>
                            <line x1="21" y1="12" x2="3" y2="12"/>
                          </svg>
                        {:else if (pageIdx * adjustPerPage + i) === 4}
                          <!-- Tilt: incline/level icon -->
                          <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                            <polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/>
                          </svg>
                        {/if}
                      </div>
                      <span class="adjust-selector-label">{prop.label}</span>
                    </button>
                  {/each}
                </div>
              {/each}
            </div>
            <!-- Page dots -->
            <div class="page-dots">
              {#each { length: adjustTotalPages() } as _, i}
                <span class="page-dot" class:active={i === adjustCurrentPage()}></span>
              {/each}
            </div>
          </div>

          <!-- Slider for selected property -->
          <div class="adjust-slider-section">
            <div class="adjust-slider-header">
              <span class="adjust-slider-prop-label">{adjustProps[selectedAdjust].label}</span>
              <span class="adjust-slider-value">{adjustProps[selectedAdjust].value}{adjustProps[selectedAdjust].unit}</span>
            </div>

            <div class="adjust-slider-controls">
              <button class="adjust-step-btn" aria-label="Decrease" onclick={() => adjustStep(selectedAdjust, -1)}>
                <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                  <line x1="5" y1="12" x2="19" y2="12"/>
                </svg>
              </button>

              <div class="adjust-slider-bar-wrap">
                <div class="adjust-slider-bar">
                  <div
                    class="adjust-slider-fill"
                    style="width: {adjustPct(adjustProps[selectedAdjust])}%"
                  ></div>
                  <div class="adjust-slider-handle current" style="left: {adjustPct(adjustProps[selectedAdjust])}%"
                    role="slider" tabindex="0"
                    aria-label={adjustProps[selectedAdjust].label}
                    aria-valuemin={adjustProps[selectedAdjust].min}
                    aria-valuemax={adjustProps[selectedAdjust].max}
                    aria-valuenow={adjustProps[selectedAdjust].value}
                    onpointerdown={(e) => adjustStartDrag(e, selectedAdjust)}
                    onpointermove={adjustOnDragMove}
                    onpointerup={adjustEndDrag}
                    onpointercancel={adjustEndDrag}
                  ></div>
                </div>
              </div>

              <button class="adjust-step-btn" aria-label="Increase" onclick={() => adjustStep(selectedAdjust, 1)}>
                <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                  <line x1="12" y1="5" x2="12" y2="19"/>
                  <line x1="5" y1="12" x2="19" y2="12"/>
                </svg>
              </button>
            </div>
          </div>
        </div>

        <!-- bottom spacer for safe area -->
        <div class="content-spacer"></div>
      </div>
      </div>
    {:else if activeTab === "skills"}
      {#if activeView === "upload"}
        <!-- ═══ Upload View ═══ -->
        <div class="upload-page">
          <div class="upload-inner">
            <UploadView onBack={() => activeView = "main"} />
          </div>
        </div>
      {:else}
        <SkillsView onNavigate={(view) => { if (view === "upload") activeView = "upload"; }} />
      {/if}
    {:else if activeTab === "store"}
      <SkillsMarketplace onNavigate={() => (activeTab = "skills")} />
    {:else if activeTab === "chat"}
      <ChatView navigate={(to) => { if (to === "home") activeTab = "home"; }} />
    {:else if activeTab === "settings"}
      {#if showWiFi}
        <WiFiSetup onNavigate={() => showWiFi = false} {network} />
      {:else if showAPConfig}
        <APModeConfig onNavigate={() => showAPConfig = false} {network} />
      {:else if showCalibration}
        <Calibration onBack={() => showCalibration = false} />
      {:else if showStudio}
        <ServoStudio onBack={() => showStudio = false} />
      {:else}
        <div class="settings-page" style="--yellow: {YELLOW}">
          <header class="settings-header" style="background: {YELLOW}">
            <h2 class="settings-title">Settings</h2>
          </header>
          <div class="settings-list">
            <button class="settings-item" onclick={() => showWiFi = true}>
              <div class="settings-item-icon">
                <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <path d="M5 12.55a11 11 0 0 1 14.08 0"/>
                  <path d="M1.42 9a16 16 0 0 1 21.16 0"/>
                  <path d="M8.53 16.11a6 6 0 0 1 6.95 0"/>
                  <circle cx="12" cy="20" r="1" fill="currentColor"/>
                </svg>
              </div>
              <div class="settings-item-content">
                <span class="settings-item-label">Wi‑Fi</span>
                <span class="settings-item-desc">Join or configure wireless network</span>
              </div>
              <svg class="settings-chevron" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <polyline points="9 18 15 12 9 6"/>
              </svg>
            </button>
            <button class="settings-item" onclick={() => showAPConfig = true}>
              <div class="settings-item-icon">
                <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <path d="M12 2a10 10 0 0 1 10 10"/>
                  <path d="M12 6a6 6 0 0 1 6 6"/>
                  <path d="M12 10a2 2 0 0 1 2 2"/>
                  <circle cx="12" cy="20" r="1.5" fill="currentColor"/>
                </svg>
              </div>
              <div class="settings-item-content">
                <span class="settings-item-label">AP Mode</span>
                <span class="settings-item-desc">Configure hotspot access point</span>
              </div>
              <svg class="settings-chevron" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <polyline points="9 18 15 12 9 6"/>
              </svg>
            </button>
            <button class="settings-item" onclick={() => showCalibration = true}>
              <div class="settings-item-icon">
                <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <circle cx="12" cy="12" r="3"/>
                  <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/>
                </svg>
              </div>
              <div class="settings-item-content">
                <span class="settings-item-label">Calibration</span>
                <span class="settings-item-desc">Adjust leg positions and offsets</span>
              </div>
              <svg class="settings-chevron" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <polyline points="9 18 15 12 9 6"/>
              </svg>
            </button>
            <button class="settings-item" onclick={() => showStudio = true}>
              <div class="settings-item-icon">
                <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <line x1="4" y1="21" x2="4" y2="14"/><line x1="4" y1="10" x2="4" y2="3"/>
                  <line x1="12" y1="21" x2="12" y2="12"/><line x1="12" y1="8" x2="12" y2="3"/>
                  <line x1="20" y1="21" x2="20" y2="16"/><line x1="20" y1="12" x2="20" y2="3"/>
                  <line x1="1" y1="14" x2="7" y2="14"/><line x1="9" y1="8" x2="15" y2="8"/>
                  <line x1="17" y1="16" x2="23" y2="16"/>
                </svg>
              </div>
              <div class="settings-item-content">
                <span class="settings-item-label">Servo Studio</span>
                <span class="settings-item-desc">Tune Kp / Kd gains on the driver boards</span>
              </div>
              <svg class="settings-chevron" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <polyline points="9 18 15 12 9 6"/>
              </svg>
            </button>
            <!-- The Skill Store row lived here. It is a bottom-nav tab now:
                 two doors to one screen is how people end up unsure which one
                 they are looking at. -->
            <!-- "Skill Management" used to live here. It listed only
                 marketplace skills, while skills pushed from mpx-cli were
                 invisible to it — two half-lists on two screens. Both are now
                 on the Skills tab, so this entry would only be a second way
                 into a screen that is already one tap away. -->
          </div>
        </div>
      {/if}
    {/if}
  </main>

  <!-- ═══ Bottom Navigation ═══ -->
  <nav class="bottom-nav">
    {#each tabs as tab}
      <button
        class="nav-item"
        class:active={activeTab === tab.id}
        onclick={() => selectTab(tab.id)}
      >
        <div class="nav-icon">
          {#if tab.id === "home"}
            <!-- Eye-open icon -->
            <svg width="37" height="37" viewBox="0 0 37 37" fill="none" xmlns="http://www.w3.org/2000/svg">
              <ellipse cx="18.1373" cy="18.5" rx="18.1373" ry="18.5" fill="#FCE505"/>
              <rect x="21.5392" y="12.8333" width="7.83094" height="10.2433" rx="3.91547" fill="black"/>
              <rect x="21.5392" y="12.8333" width="7.83094" height="10.2433" rx="3.91547" fill="black"/>
              <rect x="21.5392" y="12.8333" width="7.83094" height="10.2433" rx="3.91547" fill="black"/>
              <rect x="21.5392" y="12.8333" width="7.83094" height="10.2433" rx="3.91547" stroke="black"/>
              <rect x="7.89996" y="13" width="7.83094" height="10.2433" rx="3.91547" fill="black"/>
              <rect x="7.89996" y="13" width="7.83094" height="10.2433" rx="3.91547" fill="black"/>
              <rect x="7.89996" y="13" width="7.83094" height="10.2433" rx="3.91547" fill="black"/>
              <rect x="7.89996" y="13" width="7.83094" height="10.2433" rx="3.91547" stroke="black"/>
              <path d="M9.42374 22.8531C8.86066 23.2172 8.77736 23.5008 8.50385 24.0242H15.7403C15.3711 23.5109 15.1497 23.2409 14.5751 22.8531C14.162 22.5742 13.9236 22.3971 13.4099 22.2441C12.5844 21.998 11.6263 22.0616 10.7729 22.2441C10.123 22.383 9.93878 22.52 9.42374 22.8531Z" fill="#FCE505" stroke="#FFE605"/>
              <path d="M22.4249 22.8274C21.8618 23.1916 21.7785 23.4752 21.505 23.9986H28.7414C28.3722 23.4853 28.1508 23.2153 27.5762 22.8274C27.1631 22.5485 26.9247 22.3715 26.4111 22.2184C25.5855 21.9724 24.6274 22.036 23.774 22.2184C23.1242 22.3573 22.9399 22.4943 22.4249 22.8274Z" fill="#FFE605" stroke="#FFE605"/>
            </svg>

          {:else if tab.id === "skills"}
            <!-- Lightning bolt — a skill is something that runs, not a folder -->
            <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
              <polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"/>
            </svg>
          {:else if tab.id === "store"}
            <!-- Shopfront awning. A bag or a cart would read as a checkout;
                 this reads as a place you browse. -->
            <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
              <path d="M3 9l1.5-5h15L21 9"/>
              <path d="M4 9v10a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1V9"/>
              <path d="M3 9h18"/>
              <path d="M9 20v-6h6v6"/>
            </svg>
          {:else if tab.id === "chat"}
            <!-- Chat bubble -->
            <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
              <path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/>
            </svg>
          {:else if tab.id === "settings"}
            <!-- Gear icon -->
            <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
              <circle cx="12" cy="12" r="3"/>
              <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/>
            </svg>
          {/if}
        </div>
        <span class="nav-label">{tab.label}</span>
      </button>
    {/each}
  </nav>
</div>

<style>
  .home-root {
    position: absolute;
    inset: 0;
    background: #fff;
    display: flex;
    flex-direction: column;
  }

  /* ════════════════════════════════════════
     TOP HEADER
     ════════════════════════════════════════ */
  .top-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 10px 14px;
    flex-shrink: 0;
  }

  .header-left,
  .header-right {
    display: flex;
    align-items: center;
    gap: 6px;
  }

  .header-center {
    position: absolute;
    left: 50%;
    transform: translateX(-50%);
  }

  .header-logo {
    height: 20px;
    width: auto;
    display: block;
  }

  /* ── Connection Mode Pill ──────────────── */
  .conn-mode-pill {
    display: flex;
    align-items: center;
    gap: 5px;
    padding: 3px 10px 3px 8px;
    border-radius: 999px;
    font-size: 0.7rem;
    font-weight: 700;
    line-height: 1.4;
    background: rgba(0, 0, 0, 0.12);
    color: #000;
  }

  .conn-mode-dot {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    background: #999;
  }
  .conn-mode-pill.mode-ap .conn-mode-dot {
    background: #22c55e; /* green — AP mode is direct */
  }
  .conn-mode-pill.mode-sta .conn-mode-dot {
    background: #3b82f6; /* blue — STA mode */
  }

  .conn-mode-label {
    text-transform: uppercase;
    letter-spacing: 0.03em;
  }



  /* ════════════════════════════════════════
     CONTENT
     ════════════════════════════════════════ */
  .content {
    flex: 1;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
  }

  .home-content {
    display: flex;
    flex-direction: column;
    flex: 1;
    min-height: 0;
  }

  .home-content-scroll {
    padding: 12px 14px 24px;
    display: flex;
    flex-direction: column;
    gap: 12px;
    flex-shrink: 0;
  }

  /* ════════════════════════════════════════
     CONTROLLER PANEL
     ════════════════════════════════════════ */
  .controller-panel {
    flex: 1;
    min-height: calc(100dvh - 150px);
    display: flex;
    flex-direction: column;
    padding: 10px 14px 14px;
    gap: 0;
    position: relative;
    overflow: hidden;
  }

  /* ── White gradient overlay ────────────── */
  .controller-panel::after {
    content: '';
    position: absolute;
    inset: 0;
    background: radial-gradient(
      ellipse 85% 60% at 50% 35%,
      rgba(255,255,255,0.25) 0%,
      rgba(255,255,255,0.08) 40%,
      transparent 70%
    );
    pointer-events: none;
  }
  .controller-panel.landscape::after {
    display: none;
  }

  /* ── Fullscreen mode ──────────────────── */
  .controller-panel.fullscreen {
    position: fixed;
    inset: 0;
    z-index: 100;
    padding: 10px 14px 14px;
    border-radius: 0;
  }

  /* ── Overlay Control Buttons ──────────── */
  .ctrl-overlay-btns {
    position: absolute;
    top: 2px;
    left: 50%;
    transform: translateX(-50%);
    display: flex;
    gap: 10px;
    z-index: 20;
  }

  .ctrl-overlay-btn {
    width: 34px;
    height: 34px;
    border-radius: 50%;
    border: none;
    background: rgba(0, 0, 0, 0.2);
    color: #fff;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: background 0.15s, transform 0.1s;
    backdrop-filter: blur(4px);
    -webkit-backdrop-filter: blur(4px);
  }
  .ctrl-overlay-btn:hover {
    background: rgba(0, 0, 0, 0.35);
  }
  .ctrl-overlay-btn:active {
    transform: scale(0.88);
  }
  .ctrl-overlay-btn.active {
    background: var(--yellow, #FFE605);
    color: #000;
  }

  /* ════════════════════════════════════════
     SECTION CARDS
     ════════════════════════════════════════ */
  .section-card {
    background: #f5f5f5;
    border-radius: 16px;
    padding: 16px;
    flex-shrink: 0;
  }

  .section-card-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 12px;
  }

  .section-card-title {
    font-size: 1rem;
    font-weight: 700;
    color: #000;
  }

  .see-all-btn {
    border: none;
    background: none;
    color: #0357B7;
    font-size: 0.8rem;
    font-weight: 600;
    cursor: pointer;
    padding: 4px 8px;
    border-radius: 8px;
    transition: background 0.15s;
  }
  .see-all-btn:hover {
    background: rgba(0,0,0,0.06);
  }

  /* ── Action Grid (swipeable) ───────────── */
  .action-grid-scroll-wrap {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .action-grid-scroll {
    display: flex;
    overflow-x: auto;
    scroll-snap-type: x mandatory;
    -webkit-overflow-scrolling: touch;
    scrollbar-width: none;
    gap: 0;
    margin: -4px 0;
    padding: 4px 0;
  }
  .action-grid-scroll::-webkit-scrollbar {
    display: none;
  }

  .action-grid-page {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 12px;
    flex: 0 0 100%;
    scroll-snap-align: start;
  }

  /* ── Action Grid Item ──────────────────── */
  .action-grid-item {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 6px;
    border: none;
    background: transparent;
    cursor: pointer;
    padding: 4px 0;
    transition: transform 0.1s;
  }
  .action-grid-item:active {
    transform: scale(0.92);
  }

  .action-grid-icon {
    width: 58px;
    height: 58px;
    border-radius: 14px;
    background: var(--action-color);
    display: flex;
    align-items: center;
    justify-content: center;
    color: #fff;
    transition: box-shadow 0.15s;
  }
  .action-grid-item:hover .action-grid-icon {
    box-shadow: 0 3px 10px rgba(0,0,0,0.18);
  }

  .action-grid-icon :global(.action-emoji) {
    font-size: 2rem;
  }

  .action-grid-label {
    font-size: 0.7rem;
    font-weight: 600;
    color: #000;
    text-align: center;
    line-height: 1.2;
  }

  /* ── Pinned item trash button ─────────── */
  .pinned-item {
    position: relative;
  }

  .unpin-trash-btn {
    position: absolute;
    top: -6px;
    right: -6px;
    width: 24px;
    height: 24px;
    border-radius: 50%;
    background: #d14949;
    border: 2px solid #fff;
    display: none;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    z-index: 5;
    box-shadow: 0 2px 6px rgba(0,0,0,0.2);
    transition: transform 0.1s;
  }
  .pinned-item.show-trash .unpin-trash-btn {
    display: flex;
  }
  .unpin-trash-btn:active {
    transform: scale(0.85);
  }

  .no-pinned-hint {
    font-size: 0.8rem;
    color: #999;
    text-align: center;
    padding: 16px 8px;
    font-style: italic;
  }

  /* ── All Actions: pin & delete buttons ── */
  .all-action-item {
    position: relative;
  }

  .pin-trigger-btn {
    position: absolute;
    top: 2px;
    left: 2px;
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: rgba(0,0,0,0.4);
    border: none;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    color: #fff;
    opacity: 0.6;
    transition: opacity 0.15s, background 0.15s;
    padding: 0;
  }
  .pin-trigger-btn.pinned {
    opacity: 1;
    background: var(--yellow, #FFE605);
    color: #000;
  }
  .all-action-item:hover .pin-trigger-btn {
    opacity: 1;
  }

  .delete-action-btn {
    position: absolute;
    top: -4px;
    right: -4px;
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: #d14949;
    border: 1.5px solid #fff;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    opacity: 0;
    transition: opacity 0.15s;
    padding: 0;
    box-shadow: 0 2px 4px rgba(0,0,0,0.2);
  }
  .all-action-item:hover .delete-action-btn {
    opacity: 1;
  }

  /* ── Collapsible Gait Header ───────────── */
  .all-actions-cat-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    width: 100%;
    border: none;
    background: none;
    cursor: pointer;
    padding: 8px 16px;
    margin: 12px 0 8px;
    transition: background 0.15s;
    border-radius: 8px;
  }
  .all-actions-cat-header:hover {
    background: rgba(0,0,0,0.04);
  }

  .collapse-chevron {
    color: #888;
    transition: transform 0.2s;
    flex-shrink: 0;
  }
  .collapse-chevron.collapsed {
    transform: rotate(-90deg);
  }

  /* ── Add Item (dashed box) ─────────────── */
  .add-icon-box {
    background: transparent !important;
    border: 2.5px dashed #bbb;
    color: #bbb;
  }

  /* ── Page Dots ─────────────────────────── */
  .page-dots {
    display: flex;
    justify-content: center;
    gap: 6px;
  }

  .page-dot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: #ddd;
    transition: background 0.2s;
  }
  .page-dot.active {
    background: var(--yellow, #FFE605);
  }

  /* ── All Actions Page (full screen) ────── */
  .all-actions-page {
    flex: 1;
    display: flex;
    flex-direction: column;
    padding: 16px 14px 24px;
    overflow-y: auto;
    background: #fff;
  }
  .all-actions-page::-webkit-scrollbar {
    width: 4px;
  }
  .all-actions-page::-webkit-scrollbar-thumb {
    background: #ddd;
    border-radius: 4px;
  }

  .all-actions-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 20px;
    flex-shrink: 0;
  }

  .all-actions-title {
    font-size: 1.1rem;
    font-weight: 700;
    color: #000;
    margin: 0;
  }

  .all-actions-scroll {
    flex: 1;
    overflow-y: auto;
    padding: 8px 0 24px;
  }
  .all-actions-scroll::-webkit-scrollbar {
    width: 4px;
  }
  .all-actions-scroll::-webkit-scrollbar-thumb {
    background: #ddd;
    border-radius: 4px;
  }

  .all-actions-category {
    margin-bottom: 8px;
  }

  .all-actions-cat-title {
    font-size: 0.75rem;
    font-weight: 700;
    color: #888;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    margin: 0;
    padding: 0;
  }

  .all-actions-add-wrap {
    padding: 0 16px;
    margin-top: 8px;
  }

  .actions-full-grid {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 14px;
    padding: 4px 0;
    flex-shrink: 0;
  }

  /* ── Back Button ───────────────────────── */
  .back-btn {
    width: 32px;
    height: 32px;
    display: flex;
    align-items: center;
    justify-content: center;
    border: none;
    background: rgba(0,0,0,0.06);
    border-radius: 50%;
    cursor: pointer;
    color: #000;
    flex-shrink: 0;
    transition: background 0.15s;
  }
  .back-btn:hover {
    background: rgba(0,0,0,0.12);
  }

  /* ── Adjust Selectors (swipeable) ─────── */
  .adjust-scroll-wrap {
    display: flex;
    flex-direction: column;
    gap: 10px;
    margin-bottom: 16px;
  }

  .adjust-scroll {
    display: flex;
    overflow-x: auto;
    scroll-snap-type: x mandatory;
    -webkit-overflow-scrolling: touch;
    scrollbar-width: none;
    gap: 0;
    margin: -4px 0;
    padding: 4px 0;
  }
  .adjust-scroll::-webkit-scrollbar {
    display: none;
  }

  .adjust-page {
    display: flex;
    justify-content: space-around;
    flex: 0 0 100%;
    scroll-snap-align: start;
    gap: 8px;
  }

  .adjust-selector-btn {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 6px;
    border: none;
    background: transparent;
    cursor: pointer;
    padding: 0;
    transition: transform 0.1s;
  }
  .adjust-selector-btn:active {
    transform: scale(0.92);
  }

  .adjust-selector-circle {
    width: 52px;
    height: 52px;
    border-radius: 50%;
    background: #e8e8e8;
    display: flex;
    align-items: center;
    justify-content: center;
    color: #999;
    transition: background 0.2s, color 0.2s, box-shadow 0.2s;
  }
  .adjust-selector-circle.active {
    background: var(--sel-color, #FFE605);
    color: #000;
    box-shadow: 0 3px 10px rgba(0,0,0,0.15);
  }

  .adjust-selector-label {
    font-size: 0.7rem;
    font-weight: 600;
    color: #000;
    text-align: center;
  }

  /* ── Adjust Slider Section ─────────────── */
  .adjust-slider-section {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .adjust-slider-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0 2px;
  }

  .adjust-slider-prop-label {
    font-size: 0.85rem;
    font-weight: 600;
    color: #000;
  }

  .adjust-slider-value {
    font-size: 0.85rem;
    font-weight: 700;
    color: #000;
    font-variant-numeric: tabular-nums;
  }

  .adjust-slider-controls {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .adjust-step-btn {
    width: 32px;
    height: 32px;
    border-radius: 10px;
    border: 1.5px solid #ccc;
    background: #fff;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    color: #666;
    flex-shrink: 0;
    transition: background 0.15s, border-color 0.15s;
  }
  .adjust-step-btn:hover {
    background: #f0f0f0;
    border-color: #999;
  }

  .adjust-slider-bar-wrap {
    flex: 1;
    padding: 16px 0;
  }

  .adjust-slider-bar {
    position: relative;
    width: 100%;
    height: 5px;
    border-radius: 4px;
    background: #ddd;
  }

  .adjust-slider-fill {
    position: absolute;
    top: 0;
    height: 100%;
    border-radius: 4px;
    background: var(--yellow, #FFE605);
    pointer-events: none;
  }

  .adjust-slider-handle {
    position: absolute;
    top: 50%;
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: var(--yellow, #FFE605);
    border: 3px solid #fff;
    box-shadow: 0 1px 4px rgba(0,0,0,0.25);
    transform: translate(-50%, -50%);
    pointer-events: none;
    z-index: 1;
  }
  .adjust-slider-handle.current {
    z-index: 2;
    pointer-events: auto;
    touch-action: none;
    cursor: grab;
  }
  .adjust-slider-handle.current:active {
    cursor: grabbing;
  }

  /* ── All Adjust Page (full screen) ─────── */
  .all-adjust-page {
    flex: 1;
    display: flex;
    flex-direction: column;
    padding: 16px 14px 24px;
    overflow-y: auto;
    background: #fff;
  }
  .all-adjust-page::-webkit-scrollbar {
    width: 4px;
  }
  .all-adjust-page::-webkit-scrollbar-thumb {
    background: #ddd;
    border-radius: 4px;
  }

  .all-adjust-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 20px;
    flex-shrink: 0;
  }

  .all-adjust-title {
    font-size: 1.1rem;
    font-weight: 700;
    color: #000;
    margin: 0;
  }

  .all-adjust-list {
    display: flex;
    flex-direction: column;
    gap: 18px;
    flex-shrink: 0;
  }

  .all-adjust-item {
    background: #f5f5f5;
    border-radius: 14px;
    padding: 14px 16px;
  }

  .all-adjust-item-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 10px;
  }

  .all-adjust-item-label {
    font-size: 0.9rem;
    font-weight: 600;
    color: #000;
  }

  .all-adjust-item-value {
    font-size: 0.85rem;
    font-weight: 700;
    color: #000;
    font-variant-numeric: tabular-nums;
  }

  .all-adjust-slider-row {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 4px 0;
  }

  .all-adjust-slider-container {
    flex: 1;
    padding: 10px 0 4px;
  }

  .all-adjust-slider-bar {
    position: relative;
    width: 100%;
    height: 6px;
    border-radius: 4px;
    background: #ddd;
  }

  .all-adjust-slider-fill {
    position: absolute;
    top: 0;
    height: 100%;
    border-radius: 4px;
    background: var(--yellow, #FFE605);
    pointer-events: none;
  }

  .all-adjust-slider-handle {
    position: absolute;
    top: 50%;
    width: 18px;
    height: 18px;
    border-radius: 50%;
    background: var(--yellow, #FFE605);
    border: 3px solid #fff;
    box-shadow: 0 1px 4px rgba(0,0,0,0.25);
    transform: translate(-50%, -50%);
    pointer-events: none;
    z-index: 1;
  }
  .all-adjust-slider-handle.current {
    z-index: 2;
    pointer-events: auto;
    touch-action: none;
    cursor: grab;
  }
  .all-adjust-slider-handle.current:active {
    cursor: grabbing;
  }

  .content-spacer {
    height: 8px;
    flex-shrink: 0;
  }

  /* ════════════════════════════════════════
     BOTTOM NAV
     ════════════════════════════════════════ */
  .bottom-nav {
    display: flex;
    align-items: stretch;
    background: #fff;
    border-top: 1px solid #eee;
    padding: 6px 0 env(safe-area-inset-bottom, 6px);
    flex-shrink: 0;
  }

  .nav-item {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 2px;
    padding: 6px 0;
    border: none;
    background: transparent;
    cursor: pointer;
    color: #ccc;
    transition: color 0.15s;
  }

  .nav-item.active {
    color: #000;
  }

  .nav-icon {
    display: flex;
    align-items: center;
    justify-content: center;
    height: 28px;
  }

  .nav-label {
    font-size: 0.65rem;
    font-weight: 600;
    letter-spacing: 0.02em;
    /* Five tabs instead of four: on a 320px phone each gets about 64px and
       "Settings" is the longest label. Clip rather than wrap, so one long
       word cannot make the whole bar taller than its neighbours. */
    max-width: 100%;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  /* ════════════════════════════════════════
     UPLOAD PAGE
     ════════════════════════════════════════ */
  .upload-page {
    flex: 1;
    display: flex;
    flex-direction: column;
  }

  .upload-inner {
    flex: 1;
    display: flex;
    flex-direction: column;
  }

  /* ════════════════════════════════════════
     SETTINGS PAGE
     ════════════════════════════════════════ */
  .settings-page {
    flex: 1;
    background: #f5f5f5;
    display: flex;
    flex-direction: column;
  }

  .settings-header {
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 12px 14px;
    flex-shrink: 0;
  }

  .settings-title {
    font-size: 1.05rem;
    font-weight: 800;
    color: #000;
    margin: 0;
  }

  .settings-list {
    padding: 12px 14px;
    display: flex;
    flex-direction: column;
    gap: 8px;
    flex-shrink: 0;
  }

  .settings-item {
    display: flex;
    align-items: center;
    gap: 14px;
    width: 100%;
    padding: 16px;
    border: none;
    background: #fff;
    border-radius: 14px;
    cursor: pointer;
    text-align: left;
    transition: background 0.15s, box-shadow 0.15s;
    box-shadow: 0 1px 3px rgba(0,0,0,0.06);
  }
  .settings-item:hover {
    background: #fafafa;
    box-shadow: 0 2px 6px rgba(0,0,0,0.1);
  }
  .settings-item:active {
    background: #f0f0f0;
  }

  .settings-item-icon {
    width: 44px;
    height: 44px;
    border-radius: 12px;
    background: var(--yellow, #FFE605);
    display: flex;
    align-items: center;
    justify-content: center;
    color: #000;
    flex-shrink: 0;
  }

  .settings-item-content {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .settings-item-label {
    font-size: 0.95rem;
    font-weight: 700;
    color: #000;
  }

  .settings-item-desc {
    font-size: 0.75rem;
    font-weight: 500;
    color: #999;
  }

  .settings-chevron {
    color: #ccc;
    flex-shrink: 0;
  }
</style>
