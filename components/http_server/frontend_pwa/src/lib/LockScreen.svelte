<script>
  import { colors } from "./colors.js";
  import WelcomeScreen from "./WelcomeScreen.svelte";

  let { network } = $props();

  /** "locked" → "unlocked" → "final" */
  let phase = $state("locked");
  let dragProgress = $state(0);
  let isDragging = $state(false);

  const YELLOW = colors.mpx.primary; // #FFE605

  /* ── Pointer handlers ─────────────────── */
  function handlePointerDown(e) {
    if (isUnlocked) return;
    isDragging = true;
    e.preventDefault();
    e.currentTarget.setPointerCapture(e.pointerId);
  }

  function handlePointerMove(e) {
    if (!isDragging) return;
    const rect = e.currentTarget.getBoundingClientRect();
    const progress = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
    dragProgress = progress;
  }

  function handlePointerUp() {
    if (!isDragging) return;
    isDragging = false;

    if (dragProgress >= 0.85) {
      phase = "unlocked";
      setTimeout(() => {
        phase = "final";
      }, 1200);
    } else {
      dragProgress = 0;
    }
  }

  /* ── Derived values ───────────────────── */
  const isUnlocked = $derived(phase === "unlocked" || phase === "final");
  const isFinal = $derived(phase === "final");

  const lockIconRotation = $derived(dragProgress * -360);
  const handleX = $derived(`${dragProgress * 100}%`);
  const sliderFill = $derived(dragProgress * 100);
</script>

<div class="root" style="--brand-yellow: {YELLOW}" class:final={isFinal}>
  <!-- ═══ LOCKED / UNLOCKED SCREEN ═══ -->
  <div class="screen" class:exit={isFinal}>
    <div class="center-group">
      <!-- Eyes -->
      <div class="eyes-area">
        {#if isUnlocked}
          <!-- eye-close.svg (unlocked) -->
          <svg width="156" height="51" viewBox="0 0 156 51" fill="none">
            <rect width="64" height="42" rx="16" fill="black"/>
            <path d="M1.5 49V21.5L62.5 25V45C30.626 39.5779 18.4342 39.6692 1.5 49Z" fill="black" stroke="black" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>
            <rect x="92" width="64" height="34" rx="16" fill="black"/>
            <path d="M93.5 39.5V16H154.5V35C124 31.5 108 33.5 93.5 39.5Z" fill="black" stroke="black" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>
          </svg>
        {:else}
          <!-- eye-open.svg (locked) -->
          <svg width="181" height="124" viewBox="0 0 181 124" fill="none">
            <rect x="109.5" y="0.5" width="71" height="119" rx="17.5" fill="black"/>
            <rect x="109.5" y="0.5" width="71" height="119" rx="17.5" stroke="black"/>
            <rect x="0.5" y="0.5" width="71" height="119" rx="17.5" fill="black"/>
            <rect x="0.5" y="0.5" width="71" height="119" rx="17.5" stroke="black"/>
            <path d="M16.5 110.5C11.9092 114.387 11.23 117.413 9 123H68C64.9899 117.521 63.1846 114.639 58.5 110.5C55.1314 107.523 53.1879 105.634 49 104C42.2693 101.374 34.4575 102.053 27.5 104C22.2014 105.483 20.6992 106.945 16.5 110.5Z" fill="#FCE505" stroke="#FFE605"/>
            <path d="M122.5 110.224C117.909 114.111 117.23 117.137 115 122.724H174C170.99 117.245 169.185 114.363 164.5 110.224C161.131 107.247 159.188 105.358 155 103.724C148.269 101.098 140.457 101.777 133.5 103.724C128.201 105.207 126.699 106.669 122.5 110.224Z" fill="#FFE605" stroke="#FFE605"/>
          </svg>
        {/if}
      </div>

      <!-- Brand name -->
      <img class="brand-name" src="/md.svg" alt="MangDang" />

      <!-- Lock icon -->
      <div class="lock-icon-wrapper">
        {#if isUnlocked}
          <span class="lock-icon unlocked">🔓</span>
        {:else}
          <span class="lock-icon" style="transform: rotate({lockIconRotation}deg)">🔒</span>
        {/if}
      </div>
    </div>

    <!-- Bottom: slider -->
    <div class="slider-section">
      <div class="hint-text">
        {#if isUnlocked}
          <span class="hint-unlocked">Unlocked!</span>
        {:else}
          <span class="hint-locked">Slide to unlock</span>
        {/if}
      </div>

      <div
        class="slider-track"
        class:unlocked={isUnlocked}
        role="slider"
        tabindex="0"
        aria-label="Slide to unlock"
        aria-valuenow={Math.round(dragProgress * 100)}
        onpointerdown={handlePointerDown}
        onpointermove={handlePointerMove}
        onpointerup={handlePointerUp}
        onpointerleave={handlePointerUp}
      >
        {#if isUnlocked}
          <div class="slider-fill-full"></div>
          <div class="slider-handle checkmark-handle">
            <svg class="handle-check" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round">
              <path d="M5 13l4 4L19 7"/>
            </svg>
          </div>
        {:else}
          <div class="slider-fill" style="width: {sliderFill}%"></div>
          <div
            class="slider-handle"
            class:dragging={isDragging}
            style="left: {handleX}"
          >
            <svg class="handle-arrow" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
              <path d="M5 12h14M13 5l7 7-7 7"/>
            </svg>
          </div>
        {/if}
      </div>
    </div>
  </div>

  <!-- ═══ FINAL SCREEN → Welcome / Setup ═══ -->
  {#if isFinal}
    <WelcomeScreen {network} />
  {/if}
</div>

<style>
  /* ── Root ─────────────────────────────── */
  .root {
    position: relative;
    width: 100%;
    height: 100dvh;
    background-color: var(--brand-yellow);
    overflow: hidden;
    transition: background-color 0.8s ease;
  }

  .root.final {
    background-color: #000;
  }

  /* ── Shared Screen ────────────────────── */
  .screen {
    position: absolute;
    inset: 0;
    display: flex;
    flex-direction: column;
    align-items: center;
    padding: 0 32px 3.5em;
    justify-content: center;
    transition: opacity 0.6s ease, transform 0.6s ease;
  }

  .screen.exit {
    opacity: 0;
    transform: scale(0.96);
    pointer-events: none;
  }

  /* ── Eyes Area ────────────────────────── */
  .eyes-area {
    display: flex;
    justify-content: center;
    align-items: flex-end;
    min-height: 124px;
  }

  .eyes-area svg {
    display: block;
  }

  /* ── Brand Name ───────────────────────── */
  .brand-name {
    width: 180px;
    height: auto;
    display: block;
    margin: 24px auto 0;
  }

  /* ── Lock Icon ────────────────────────── */
  .lock-icon-wrapper {
    margin-top: 16px;
    height: 32px;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .lock-icon {
    display: inline-block;
    font-size: 1.6rem;
    line-height: 1;
    transition: transform 0.1s linear;
  }

  .lock-icon.unlocked {
    font-size: 1.8rem;
  }

  /* ── Spacer ───────────────────────────── */
  .center-group {
    display: flex;
    flex-direction: column;
    align-items: center;
    flex: 1;
    justify-content: center;
  }

  .slider-section {
    width: 100%;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 4px;
    padding-bottom: 2em;
  }

  .hint-text {
    font-size: 0.85rem;
    font-weight: 600;
    letter-spacing: 0.08em;
  }

  .hint-locked {
    color: rgba(0, 0, 0, 0.5);
  }

  .hint-locked::before {
    content: "🔒 ";
    font-size: 0.75rem;
  }

  .hint-unlocked {
    color: #000;
  }

  .hint-unlocked::before {
    content: "🔓 ";
    font-size: 0.75rem;
  }

  /* ── Slider Track ─────────────────────── */
  .slider-track {
    position: relative;
    width: 100%;
    max-width: 280px;
    height: 44px;
    border-radius: 24px;
    touch-action: none;
    user-select: none;
    transition: background-color 0.3s ease;
    cursor: grab;
  }

  .slider-track:active {
    cursor: grabbing;
  }

  .slider-track:not(.unlocked) {
    background: rgba(0, 0, 0, 0.1);
  }

  .slider-track.unlocked {
    background: #000;
    cursor: default;
  }

  .slider-fill {
    position: absolute;
    left: 0;
    top: 0;
    height: 100%;
    background: rgba(0, 0, 0, 0.18);
    border-radius: 24px;
    pointer-events: none;
    transition: width 0.05s linear;
  }

  .slider-fill-full {
    position: absolute;
    inset: 0;
    background: #000;
    border-radius: 24px;
  }

  /* ── Slider Handle ────────────────────── */
  .slider-handle {
    position: absolute;
    top: 50%;
    transform: translate(-50%, -50%);
    width: 38px;
    height: 38px;
    border-radius: 50%;
    background: #fff;
    box-shadow:
      0 2px 8px rgba(0, 0, 0, 0.2),
      0 1px 2px rgba(0, 0, 0, 0.1);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 2;
    will-change: left;
  }

  .slider-handle:not(.checkmark-handle) {
    left: 0;
    transition: left 0.05s linear, box-shadow 0.2s ease, transform 0.2s ease;
  }

  .slider-handle.dragging {
    box-shadow:
      0 4px 16px rgba(0, 0, 0, 0.3),
      0 2px 4px rgba(0, 0, 0, 0.15);
    transform: translate(-50%, -50%) scale(1.08);
  }

  .checkmark-handle {
    left: calc(100% - 19px);
    transition: none;
  }

  .handle-arrow {
    width: 18px;
    height: 18px;
    color: #000;
  }

  .handle-check {
    width: 20px;
    height: 20px;
    color: #000;
  }

</style>
