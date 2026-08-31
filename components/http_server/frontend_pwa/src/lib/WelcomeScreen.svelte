<script>
  import { colors } from "./colors.js";
  import WiFiSetup from "./WiFiSetup.svelte";
  import APModeConfig from "./APModeConfig.svelte";
  import HomeScreen from "./HomeScreen.svelte";

  let { network } = $props();

  const YELLOW = colors.mpx.primary;

  /** Current view: "welcome" | "wifi" | "ap" | "home" */
  let view = $state("welcome");

  // ── Derived network state ──────────────────────────────────
  let isStaConnected = $derived(network?.staState === "connected");
  let displayIp = $derived(network?.apIp || "192.168.2.1");
  let staLabel = $derived(
    isStaConnected ? (network?.staSsid || "Connected")
    : network?.staState === "connecting" ? "Connecting…"
    : "OFF"
  );

  function onNavigate(next) {
    view = next;
  }

  function goToWifi() {
    view = "wifi";
  }

  function goToAP() {
    view = "ap";
  }
</script>

<div class="welcome-root" style="--brand-yellow: {YELLOW}">
  {#if view === "home"}
    <HomeScreen {network} />
  {:else if view === "wifi"}
    <WiFiSetup {onNavigate} {network} />
  {:else if view === "ap"}
    <APModeConfig {onNavigate} {network} />
  {:else}
    <!-- ═══ WELCOME VIEW ═══ -->
    <div class="welcome-view">
      <!-- Header -->
      <div class="welcome-header">
        <img class="brand-title" src="/md.svg" alt="MangDang" />
      </div>

      <!-- Your Pupper Card -->
      <div class="pupper-card" style="background: {YELLOW}">
        <div class="pupper-card-top">
          <div class="pupper-avatar">
            <img class="avatar-eye" src="/eye-open.svg" alt="Pupper" />
          </div>
          <div class="pupper-info">
            <span class="pupper-label">Your Pupper</span>
            <span class="pupper-nickname">My MPX Dog</span>
          </div>
        </div>
        <div class="pupper-ip">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <circle cx="12" cy="12" r="10"/>
            <line x1="2" y1="12" x2="22" y2="12"/>
            <path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z"/>
          </svg>
          <span>IP: {displayIp}</span>
        </div>
      </div>

      <!-- Connection Section -->
      <div class="connection-section">
        <h2 class="section-title">Connection</h2>

        <div class="connection-list">
          <!-- LAN -->
          <div class="conn-item">
            <div class="conn-left">
              <div class="conn-icon conn-icon-lan">
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <rect x="2" y="2" width="20" height="8" rx="2" ry="2"/>
                  <rect x="2" y="14" width="20" height="8" rx="2" ry="2"/>
                  <line x1="6" y1="6" x2="6.01" y2="6"/>
                  <line x1="6" y1="18" x2="6.01" y2="18"/>
                </svg>
              </div>
              <div class="conn-text">
                <span class="conn-name">LAN</span>
                <span class="conn-detail">Connected</span>
              </div>
            </div>
            <div class="conn-status">
              <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="#22c55e" stroke-width="3" stroke-linecap="round" stroke-linejoin="round">
                <path d="M5 13l4 4L19 7"/>
              </svg>
            </div>
          </div>

          <!-- Wi-Fi -->
          <button class="conn-item conn-clickable" onclick={goToWifi}>
            <div class="conn-left">
              <div class="conn-icon conn-icon-wifi">
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <path d="M5 12.55a11 11 0 0 1 14.08 0"/>
                  <path d="M1.42 9a16 16 0 0 1 21.16 0"/>
                  <path d="M8.53 16.11a6 6 0 0 1 6.95 0"/>
                  <circle cx="12" cy="20" r="1" fill="currentColor"/>
                </svg>
              </div>
              <div class="conn-text">
                <span class="conn-name">Wi‑Fi</span>
                <span class="conn-detail">{staLabel}</span>
              </div>
            </div>
            <div class="conn-arrow">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="#999" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <path d="M9 18l6-6-6-6"/>
              </svg>
            </div>
          </button>

          <!-- AP Mode -->
          <button class="conn-item conn-clickable" onclick={goToAP}>
            <div class="conn-left">
              <div class="conn-icon conn-icon-ap">
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <path d="M12 2a10 10 0 0 1 10 10"/>
                  <path d="M12 6a6 6 0 0 1 6 6"/>
                  <path d="M12 10a2 2 0 0 1 2 2"/>
                  <circle cx="12" cy="20" r="1.5" fill="currentColor"/>
                </svg>
              </div>
              <div class="conn-text">
                <span class="conn-name">AP Mode</span>
                <span class="conn-detail">{network?.apSsid || "MPX-Dog"} · {displayIp}</span>
              </div>
            </div>
            <div class="conn-arrow">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="#999" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <path d="M9 18l6-6-6-6"/>
              </svg>
            </div>
          </button>
        </div>
      </div>

      <!-- Start Button -->
      <button class="start-btn" style="background: {YELLOW}" onclick={() => view = "home"}>
        Start
      </button>
    </div>
  {/if}
</div>

<style>
  .welcome-root {
    position: absolute;
    inset: 0;
    background: #fff;
    display: flex;
    flex-direction: column;
  }

  .welcome-view {
    display: flex;
    flex-direction: column;
    height: 100%;
    padding: 0 20px;
    overflow-y: auto;
  }

  /* ── Header ─────────────────────────────── */
  .welcome-header {
    padding: 20px 0 16px;
  }

  .brand-title {
    width: 140px;
    height: auto;
    display: block;
  }

  /* ── Pupper Card ────────────────────────── */
  .pupper-card {
    border-radius: 16px;
    padding: 20px;
    display: flex;
    flex-direction: column;
    gap: 12px;
  }

  .pupper-card-top {
    display: flex;
    align-items: center;
    gap: 14px;
  }

  .pupper-avatar {
    flex-shrink: 0;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .avatar-eye {
    width: 52px;
    height: auto;
    display: block;
  }

  .pupper-info {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .pupper-label {
    font-size: 0.8rem;
    font-weight: 600;
    color: rgba(0,0,0,0.6);
    text-transform: uppercase;
    letter-spacing: 0.05em;
  }

  .pupper-nickname {
    font-size: 1.15rem;
    font-weight: 700;
    color: #000;
  }

  .pupper-ip {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: 0.85rem;
    font-weight: 500;
    color: rgba(0,0,0,0.7);
  }

  /* ── Connection Section ─────────────────── */
  .connection-section {
    margin-top: 24px;
  }

  .section-title {
    font-size: 1rem;
    font-weight: 700;
    color: #000;
    margin-bottom: 12px;
  }

  .connection-list {
    display: flex;
    flex-direction: column;
    gap: 2px;
    background: #f5f5f5;
    border-radius: 14px;
    overflow: hidden;
  }

  .conn-item {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 14px 16px;
    background: #f5f5f5;
    transition: background 0.15s;
  }

  .conn-clickable {
    cursor: pointer;
    border: none;
    text-align: left;
    width: 100%;
    font: inherit;
    color: inherit;
  }
  .conn-clickable:hover {
    background: #eee;
  }
  .conn-clickable:active {
    background: #e5e5e5;
  }

  .conn-left {
    display: flex;
    align-items: center;
    gap: 12px;
  }

  .conn-icon {
    width: 36px;
    height: 36px;
    border-radius: 10px;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }

  .conn-icon-lan {
    background: rgba(0,0,0,0.08);
    color: #000;
  }

  .conn-icon-wifi {
    background: rgba(0,0,0,0.08);
    color: #000;
  }

  .conn-icon-ap {
    background: rgba(0,0,0,0.08);
    color: #000;
  }

  .conn-text {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .conn-name {
    font-size: 1rem;
    font-weight: 600;
    color: #000;
  }

  .conn-detail {
    font-size: 0.8rem;
    color: #888;
  }

  .conn-status {
    display: flex;
    align-items: center;
  }

  .conn-arrow {
    display: flex;
    align-items: center;
  }

  /* ── Start Button ───────────────────────── */
  .start-btn {
    margin: auto 0 24px;
    width: 100%;
    padding: 14px;
    border: none;
    border-radius: 12px;
    color: #000;
    font-size: 1.05rem;
    font-weight: 700;
    cursor: pointer;
    transition: transform 0.1s, box-shadow 0.15s;
  }
  .start-btn:active {
    transform: scale(0.98);
  }
</style>
