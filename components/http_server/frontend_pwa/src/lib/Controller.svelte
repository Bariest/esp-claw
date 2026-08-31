<script>
  import { colors } from "./colors.js";

  const YELLOW = colors.mpx.primary;

  /** Fraction of joystick radius that is a deadzone (0–1) */
  const DEADZONE = 0.18;
  const LOOK_DEADZONE = 0.35;

  let { assignments = {} } = $props();

  // ── Joystick state ──────────────────────────────────────────
  let joystickTopX = $state(0);
  let joystickTopY = $state(0);
  let joystickBotX = $state(0);
  let joystickBotY = $state(0);
  let joystickTopDx = $state(0);
  let joystickTopDy = $state(0);
  let joystickBotDx = $state(0);
  let joystickBotDy = $state(0);
  let activeJoystick = $state(null); // 'top' | 'bottom' | null

  // Joystick geometry (set on pointerdown)
  let jCenterX = $state(0);
  let jCenterY = $state(0);
  let jMaxRadius = $state(0);

  // ── Button press state ──────────────────────────────────────
  let pressedButtons = $state(new Set());

  // ── Robot API: Joystick values & timer ──────────────────────
  let joyF = 0, joyS = 0, joyT = 0;
  let joyTimer = null;

  async function sendJoy() {
    try {
      await fetch("/v1/robot/joy", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ f: joyF, s: joyS, t: joyT }),
      });
    } catch { /* transient — keep trying while pad is held */ }
  }

  function joyStartTimer() {
    if (!joyTimer) {
      sendJoy();
      joyTimer = setInterval(sendJoy, 100);
    }
  }

  function joyStopIfIdle() {
    if (joyF === 0 && joyS === 0 && joyT === 0 && joyTimer) {
      clearInterval(joyTimer);
      joyTimer = null;
      sendJoy(); // final zeros -> step in place
    }
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

  // ── Look direction (head pose) mapping ──────────────────────
  /** Map joystick normalised (-1..1) position to a look gait command */
  function joystickToLookMode(nx, ny) {
    const dist = Math.sqrt(nx * nx + ny * ny);
    if (dist < LOOK_DEADZONE) return 'init';

    const angle = Math.atan2(ny, nx) * 180 / Math.PI;

    if (angle > -22.5 && angle <= 22.5) return 'lookright';
    if (angle > 22.5 && angle <= 67.5) return 'looklr';
    if (angle > 67.5 && angle <= 112.5) return 'lookdown';
    if (angle > 112.5 && angle <= 157.5) return 'lookll';
    if (angle > 157.5 || angle <= -157.5) return 'lookleft';
    if (angle > -157.5 && angle <= -112.5) return 'lookul';
    if (angle > -112.5 && angle <= -67.5) return 'lookup';
    if (angle > -67.5 && angle <= -22.5) return 'lookur';

    return 'init';
  }

  let lastLookMode = $state('init');

  function sendLookMode(mode) {
    if (mode === lastLookMode) return;
    lastLookMode = mode;
    sendGait(mode);
  }

  // ── Keyboard state ──────────────────────────────────────────
  const keyMap = {
    'ArrowUp':    'dpad-up',
    'ArrowDown':  'dpad-down',
    'ArrowLeft':  'dpad-left',
    'ArrowRight': 'dpad-right',
    'w':          'dpad-up',
    's':          'dpad-down',
    'a':          'dpad-left',
    'd':          'dpad-right',
    'i':          'action-top',
    'l':          'action-right',
    'k':          'action-bottom',
    'j':          'action-left',
    'q':          'shoulder-l',
    'e':          'shoulder-r',
  };

  // ── Joystick helpers ────────────────────────────────────────

  function getJoystickClientPos(e) {
    if (e.touches && e.touches.length > 0) {
      return { x: e.touches[0].clientX, y: e.touches[0].clientY };
    }
    return { x: e.clientX, y: e.clientY };
  }

  function getJoystickRingRect(ringEl) {
    return ringEl.getBoundingClientRect();
  }

  function computeJoystickGeometry(ringEl) {
    const rect = ringEl.getBoundingClientRect();
    jCenterX = rect.left + rect.width / 2;
    jCenterY = rect.top + rect.height / 2;
    // The thumb is ~48% of ring width; so max radius = ring half - thumb half - 2px
    const thumbWidth = rect.width * 0.48;
    jMaxRadius = rect.width / 2 - thumbWidth / 2 - 2;
  }

  function startJoystick(e, which, ringEl) {
    e.preventDefault();
    activeJoystick = which;
    computeJoystickGeometry(ringEl);
    updateJoystick(e);

    // Global listeners
    document.addEventListener('mousemove', onJoystickMove);
    document.addEventListener('mouseup', endJoystick);
    document.addEventListener('touchmove', onJoystickMove, { passive: false });
    document.addEventListener('touchend', endJoystick);
    document.addEventListener('touchcancel', endJoystick);
  }

  function onJoystickMove(e) {
    e.preventDefault();
    if (!activeJoystick) return;
    updateJoystick(e);
  }

  function endJoystick() {
    if (!activeJoystick) return;
    if (activeJoystick === 'top') {
      joystickTopX = 0;
      joystickTopY = 0;
      joystickTopDx = 0;
      joystickTopDy = 0;
      joyF = 0; joyS = 0;
      // Explicitly stop the gait — zero joy values alone don't halt walking
      sendGait('none');
    } else {
      joystickBotX = 0;
      joystickBotY = 0;
      joystickBotDx = 0;
      joystickBotDy = 0;
      // Center the head on release
      sendLookMode('init');
    }
    activeJoystick = null;
    joyStopIfIdle();

    document.removeEventListener('mousemove', onJoystickMove);
    document.removeEventListener('mouseup', endJoystick);
    document.removeEventListener('touchmove', onJoystickMove);
    document.removeEventListener('touchend', endJoystick);
    document.removeEventListener('touchcancel', endJoystick);
  }

  function updateJoystick(e) {
    const pos = getJoystickClientPos(e);
    let dx = pos.x - jCenterX;
    let dy = pos.y - jCenterY;
    const dist = Math.sqrt(dx * dx + dy * dy);

    if (dist > jMaxRadius) {
      dx = (dx / dist) * jMaxRadius;
      dy = (dy / dist) * jMaxRadius;
    }

    let nx = jMaxRadius > 0 ? +(dx / jMaxRadius).toFixed(4) : 0;
    let ny = jMaxRadius > 0 ? +(dy / jMaxRadius).toFixed(4) : 0;

    // ── deadzone: snap small deflections to zero ──────────────
    const dz = activeJoystick === 'top' ? DEADZONE : LOOK_DEADZONE;
    const mag = Math.sqrt(nx * nx + ny * ny);
    if (mag < dz) {
      nx = 0;
      ny = 0;
      dx = 0;
      dy = 0;
    }

    if (activeJoystick === 'top') {
      joystickTopX = nx;
      joystickTopY = ny;
      joystickTopDx = dx;
      joystickTopDy = dy;
      // Left pad: forward/strafe
      joyF = -ny;
      joyS = -nx;
      if (mag < DEADZONE) {
        joyStopIfIdle();
      } else {
        joyStartTimer();
      }
    } else {
      joystickBotX = nx;
      joystickBotY = ny;
      joystickBotDx = dx;
      joystickBotDy = dy;
      // Right pad: head pose (look direction)
      const mode = joystickToLookMode(nx, ny);
      sendLookMode(mode);
    }
  }

  // ── Button handlers ─────────────────────────────────────────

  /** Map controller button IDs to robot gait commands */
  const buttonGaitMap = {
    'dpad-up':    'advance',
    'dpad-down':  'back',
    'dpad-left':  'left',
    'dpad-right': 'right',
    'action-top':    'jump',
    'action-right':  'twerk',
    'action-bottom': 'sit',
    'action-left':   'stretch',
    'shoulder-l': 'turnL',
    'shoulder-r': 'turnR',
  };

  function handlePress(action) {
    if (pressedButtons.has(action)) return;
    pressedButtons.add(action);
    // Check action assignments first, fall back to hardcoded map
    const gait = assignments[action] || buttonGaitMap[action];
    if (gait) sendGait(gait);
  }

  function handleRelease(action) {
    if (!pressedButtons.has(action)) return;
    pressedButtons.delete(action);
    // stop the gait when the button is released (hold-to-action)
    const gait = assignments[action] || buttonGaitMap[action];
    if (gait) sendGait('none');
  }

  function onButtonPointerDown(e, action) {
    e.preventDefault();
    handlePress(action);
    e.currentTarget.setPointerCapture(e.pointerId);
  }

  function onButtonPointerUp(e, action) {
    handleRelease(action);
  }

  function onButtonPointerLeave(e, action) {
    handleRelease(action);
  }

  // ── Keyboard handlers ───────────────────────────────────────

  function onKeyDown(e) {
    const action = keyMap[e.key];
    if (!action) return;
    e.preventDefault();
    handlePress(action);
  }

  function onKeyUp(e) {
    const action = keyMap[e.key];
    if (!action) return;
    e.preventDefault();
    handleRelease(action);
  }


</script>

<svelte:window onkeydown={onKeyDown} onkeyup={onKeyUp} />

<div class="controller-wrapper">
  <!-- ═══ Shoulder Buttons ═══ -->
  <button
    class="shoulder-btn shoulder-left"
    class:pressed={pressedButtons.has('shoulder-l')}
    onpointerdown={(e) => onButtonPointerDown(e, 'shoulder-l')}
    onpointerup={(e) => onButtonPointerUp(e, 'shoulder-l')}
    onpointerleave={(e) => onButtonPointerLeave(e, 'shoulder-l')}
    aria-label="L (shoulder)"
  >
    <svg viewBox="10 5 155 95" class="shoulder-svg" preserveAspectRatio="xMidYMid meet">
      <path d="M125.5 15C141.017 15 154.442 23.9488 160.901 36.9658C132.707 47.9067 109.171 68.1823 94.0859 94H54.5C32.6848 94 15 76.3152 15 54.5C15 32.6848 32.6848 15 54.5 15H125.5Z" fill="#E2CD18"/>
      <path d="M50.2541 58.829C50.1225 55.7138 50.6545 52.6062 51.815 49.7121C52.9757 46.8179 54.738 44.2025 56.9858 42.0412C59.2337 39.8801 61.9162 38.2217 64.8537 37.1755C67.7915 36.1294 70.9194 35.7194 74.0276 35.9735C77.1354 36.2276 80.1543 37.1391 82.8829 38.6483C85.6117 40.1576 87.9891 42.2306 89.8562 44.7283C91.7231 47.2259 93.0371 50.0922 93.7123 53.1364C94.3876 56.1809 94.4081 59.3347 93.7721 62.3876L92.9566 66.3032L88.5994 65.3957C90.2791 61.7338 90.7467 57.4888 89.6343 53.2904C87.0884 43.6809 77.2343 37.9548 67.6248 40.5005C67.1385 40.6294 66.6623 40.7783 66.1965 40.9436L66.1953 40.9429C66.1688 40.9524 66.1428 40.9633 66.1164 40.9729C57.4264 44.1025 52.4253 53.4148 54.8349 62.5101C54.8463 62.5529 54.8583 62.5955 54.8699 62.6382L54.4191 62.6566L50.4231 62.8263L50.2541 58.829Z" fill="#FFF8F8"/>
      <path d="M59.2261 56.8478L52.2236 63.8453L45.4111 58.3804" stroke="white" stroke-width="4" stroke-linecap="round"/>
    </svg>
  </button>
  <button
    class="shoulder-btn shoulder-right"
    class:pressed={pressedButtons.has('shoulder-r')}
    onpointerdown={(e) => onButtonPointerDown(e, 'shoulder-r')}
    onpointerup={(e) => onButtonPointerUp(e, 'shoulder-r')}
    onpointerleave={(e) => onButtonPointerLeave(e, 'shoulder-r')}
    aria-label="R (shoulder)"
  >
    <svg viewBox="255 5 155 95" class="shoulder-svg" preserveAspectRatio="xMidYMid meet">
      <path d="M366.5 15C388.315 15 406 32.6848 406 54.5C406 76.3152 388.315 94 366.5 94H323.914C309.28 68.9542 286.693 49.1237 259.615 37.9717C265.871 24.4115 279.586 15 295.5 15H366.5Z" fill="#E2CD18"/>
      <path d="M369.686 58.829C369.818 55.7138 369.285 52.6062 368.125 49.7121C366.964 46.8179 365.202 44.2025 362.954 42.0412C360.706 39.8801 358.024 38.2217 355.086 37.1755C352.149 36.1294 349.021 35.7194 345.912 35.9735C342.805 36.2276 339.786 37.1391 337.057 38.6483C334.328 40.1576 331.951 42.2306 330.084 44.7283C328.217 47.2259 326.903 50.0922 326.228 53.1364C325.552 56.1809 325.532 59.3347 326.168 62.3876L326.983 66.3032L331.34 65.3966C329.66 61.7346 329.193 57.4891 330.306 53.2904C332.852 43.6809 342.706 37.9548 352.315 40.5005C352.802 40.6294 353.278 40.7783 353.744 40.9436L353.745 40.9429C353.771 40.9524 353.797 40.9633 353.824 40.9729C362.514 44.1025 367.515 53.4148 365.105 62.5101C365.094 62.5529 365.082 62.5955 365.07 62.6382L365.521 62.6566L369.517 62.8263L369.686 58.829Z" fill="#FFF8F8"/>
      <path d="M360.714 56.8478L367.716 63.8453L374.529 58.3804" stroke="white" stroke-width="4" stroke-linecap="round"/>
    </svg>
  </button>

  <!-- ═══ Controller Body ═══ -->
  <div class="controller-body">
    <!-- Decorative cross backgrounds (behind D-Pad and Action buttons) -->
    <div class="bg-cross dpad-bg-cross">
      <div class="cross-bar cross-bar-h"></div>
      <div class="cross-bar cross-bar-v"></div>
    </div>
    <div class="bg-cross action-bg-cross">
      <div class="cross-bar cross-bar-h"></div>
      <div class="cross-bar cross-bar-v"></div>
    </div>

    <!-- Left Analog Joystick (Top) -->
    <!-- svelte-ignore a11y_interactive_supports_focus a11y_role_has_required_aria_props -->
    <div
      class="joystick-zone joystick-top"
      class:active={activeJoystick === 'top'}
      onpointerdown={(e) => {
        const ring = e.currentTarget.querySelector('.joystick-ring');
        startJoystick(e, 'top', ring);
      }}
      role="slider"
      aria-label="Left joystick"
      aria-valuenow={Math.round((Math.abs(joystickTopX) + Math.abs(joystickTopY)) * 50)}
      aria-valuetext={`x:${joystickTopX.toFixed(2)} y:${joystickTopY.toFixed(2)}`}
    >
      <div class="joystick-inner">
        <div class="joystick-ring">
          <div class="deadzone-indicator" style="--dz-pct:{DEADZONE * 100}%"></div>
          <div
            class="joystick-thumb"
            style="transform: translate(calc(-50% + {joystickTopDx}px), calc(-50% + {joystickTopDy}px))"
          ></div>
        </div>
      </div>
    </div>

    <!-- D-Pad -->
    <div class="dpad">
      <button
        class="dpad-btn"
        class:pressed={pressedButtons.has('dpad-up')}
        onpointerdown={(e) => onButtonPointerDown(e, 'dpad-up')}
        onpointerup={(e) => onButtonPointerUp(e, 'dpad-up')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'dpad-up')}
        aria-label="D-Pad Up"
      >
        <svg viewBox="0 0 24 24">
          <polygon points="12,3 21,20 3,20" fill="#222"/>
        </svg>
      </button>
      <button
        class="dpad-btn"
        class:pressed={pressedButtons.has('dpad-left')}
        onpointerdown={(e) => onButtonPointerDown(e, 'dpad-left')}
        onpointerup={(e) => onButtonPointerUp(e, 'dpad-left')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'dpad-left')}
        aria-label="D-Pad Left"
      >
        <svg viewBox="0 0 24 24">
          <polygon points="3,12 20,3 20,21" fill="#222"/>
        </svg>
      </button>
      <button
        class="dpad-btn"
        class:pressed={pressedButtons.has('dpad-right')}
        onpointerdown={(e) => onButtonPointerDown(e, 'dpad-right')}
        onpointerup={(e) => onButtonPointerUp(e, 'dpad-right')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'dpad-right')}
        aria-label="D-Pad Right"
      >
        <svg viewBox="0 0 24 24">
          <polygon points="21,12 4,3 4,21" fill="#222"/>
        </svg>
      </button>
      <button
        class="dpad-btn"
        class:pressed={pressedButtons.has('dpad-down')}
        onpointerdown={(e) => onButtonPointerDown(e, 'dpad-down')}
        onpointerup={(e) => onButtonPointerUp(e, 'dpad-down')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'dpad-down')}
        aria-label="D-Pad Down"
      >
        <svg viewBox="0 0 24 24">
          <polygon points="12,21 3,4 21,4" fill="#222"/>
        </svg>
      </button>
    </div>

    <!-- Action Buttons -->
    <div class="action-btns">
      <button
        class="action-btn"
        class:pressed={pressedButtons.has('action-top')}
        onpointerdown={(e) => onButtonPointerDown(e, 'action-top')}
        onpointerup={(e) => onButtonPointerUp(e, 'action-top')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'action-top')}
        aria-label="Action Top (Triangle)"
      >
        <svg viewBox="0 0 50 50" class="action-svg">
          <path d="M25 6 L40 34.5 L10 34.5 Z" fill="none" stroke="#6AAE6C" stroke-width="4" stroke-linejoin="round"/>
        </svg>
      </button>
      <button
        class="action-btn"
        class:pressed={pressedButtons.has('action-right')}
        onpointerdown={(e) => onButtonPointerDown(e, 'action-right')}
        onpointerup={(e) => onButtonPointerUp(e, 'action-right')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'action-right')}
        aria-label="Action Right (Circle)"
      >
        <svg viewBox="0 0 50 50" class="action-svg">
          <circle cx="25" cy="25" r="15" fill="none" stroke="#ED7676" stroke-width="4"/>
        </svg>
      </button>
      <button
        class="action-btn"
        class:pressed={pressedButtons.has('action-bottom')}
        onpointerdown={(e) => onButtonPointerDown(e, 'action-bottom')}
        onpointerup={(e) => onButtonPointerUp(e, 'action-bottom')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'action-bottom')}
        aria-label="Action Bottom (Pentagon)"
      >
        <svg viewBox="0 0 50 50" class="action-svg">
          <polygon points="25,10 39.3,20.4 33.8,37.1 16.2,37.1 10.7,20.4" fill="none" stroke="#89B0DB" stroke-width="4" stroke-linejoin="round"/>
        </svg>
      </button>
      <button
        class="action-btn"
        class:pressed={pressedButtons.has('action-left')}
        onpointerdown={(e) => onButtonPointerDown(e, 'action-left')}
        onpointerup={(e) => onButtonPointerUp(e, 'action-left')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'action-left')}
        aria-label="Action Left (Square)"
      >
        <svg viewBox="0 0 50 50" class="action-svg">
          <rect x="13" y="13" width="24" height="24" rx="3" fill="none" stroke="#A476ED" stroke-width="4"/>
        </svg>
      </button>
    </div>

    <!-- Right Analog Joystick (Bottom) -->
    <!-- svelte-ignore a11y_interactive_supports_focus a11y_role_has_required_aria_props -->
    <div
      class="joystick-zone joystick-bottom"
      class:active={activeJoystick === 'bottom'}
      onpointerdown={(e) => {
        const ring = e.currentTarget.querySelector('.joystick-ring');
        startJoystick(e, 'bottom', ring);
      }}
      role="slider"
      aria-label="Right joystick"
      aria-valuenow={Math.round((Math.abs(joystickBotX) + Math.abs(joystickBotY)) * 50)}
      aria-valuetext={`x:${joystickBotX.toFixed(2)} y:${joystickBotY.toFixed(2)}`}
    >
      <div class="joystick-inner">
        <div class="joystick-ring">
          <div class="deadzone-indicator" style="--dz-pct:{LOOK_DEADZONE * 100}%"></div>
          <div
            class="joystick-thumb"
            style="transform: translate(calc(-50% + {joystickBotDx}px), calc(-50% + {joystickBotDy}px))"
          ></div>
        </div>
      </div>
    </div>

  </div>
</div>

<style>
  .controller-wrapper {
    position: relative;
    width: 100%;
    flex: 1;
    min-height: 460px;
    max-width: 400px;
    align-self: center;
  }

  /* ════════════════════════════════════════
     SHOULDER BUTTONS
     ════════════════════════════════════════ */
  .shoulder-btn {
    position: absolute;
    border: none;
    background: none;
    cursor: pointer;
    padding: 0;
    line-height: 0;
    transition: transform 0.08s ease;
    z-index: 10;
  }

  .shoulder-btn .shoulder-svg {
    display: block;
    width: 100%;
    height: auto;
    filter: drop-shadow(0 4px 0 #8f7a00) drop-shadow(0 6px 12px rgba(0, 0, 0, 0.3));
  }

  .shoulder-left {
    left: 1.5%;
    top: 0.5%;
    width: 34.76%;
  }

  .shoulder-right {
    right: 1.5%;
    top: 0.5%;
    width: 34.76%;
  }

  .shoulder-btn.pressed,
  .shoulder-btn:active {
    transform: translateY(4px);
  }

  .shoulder-btn.pressed .shoulder-svg,
  .shoulder-btn:active .shoulder-svg {
    filter: drop-shadow(0 1px 0 #8f7a00) drop-shadow(0 4px 8px rgba(0, 0, 0, 0.3));
  }

  /* ════════════════════════════════════════
     CONTROLLER BODY
     ════════════════════════════════════════ */
  .controller-body {
    position: absolute;
    inset: 0;
    background: #F3DD12;
    background-image: radial-gradient(
      circle at 50% 60%,
      rgba(255, 255, 255, 0.35) 0%,
      rgba(255, 255, 255, 0) 70%
    );
    border-radius: clamp(24px, 5.5vw, 48px);
    box-shadow:
      0 8px 0 #b8a000,
      0 12px 40px rgba(0, 0, 0, 0.5),
      inset 0 2px 4px rgba(255, 255, 255, 0.4);
    border: 3px solid #d4c00e;
  }

  /* ════════════════════════════════════════
     DECORATIVE CROSS BACKGROUNDS
     ════════════════════════════════════════ */
  .bg-cross {
    position: absolute;
    pointer-events: none;
    z-index: 0;
  }

  .dpad-bg-cross {
    left: 4.05%;
    top: 40.29%;
    width: 38.71%;
    aspect-ratio: 162.588 / 166.554;
  }

  .action-bg-cross {
    right: 7.14%;
    top: 40.29%;
    width: 38.57%;
    aspect-ratio: 1;
  }

  .cross-bar {
    position: absolute;
    border-radius: 16%;
    background: radial-gradient(circle at 50% 50%, #CDBA01 0%, #B8A701 100%);
  }

  .cross-bar-h {
    width: 100%;
    height: 36%;
    top: 50%;
    left: 0;
    transform: translateY(-50%);
  }

  .cross-bar-v {
    width: 36%;
    height: 100%;
    top: 0;
    left: 50%;
    transform: translateX(-50%);
  }

  /* ════════════════════════════════════════
     ANALOG JOYSTICKS
     ════════════════════════════════════════ */
  .joystick-zone {
    position: absolute;
    width: 54.52%;
    aspect-ratio: 1;
    border-radius: 50%;
    left: 50%;
    transform: translateX(-50%);
    display: flex;
    align-items: center;
    justify-content: center;
    background: radial-gradient(circle, rgba(255,252,228,1) 75%, rgba(222,201,0,0.6) 100%);
    box-shadow: inset 0 0 30px rgba(0, 0, 0, 0.08);
    cursor: pointer;
    touch-action: none;
    z-index: 5;
  }

  .joystick-top {
    top: 4%;
  }

  .joystick-bottom {
    bottom: 3%;
  }

  .joystick-inner {
    width: 84.7%;
    aspect-ratio: 1;
    border-radius: 50%;
    background: #FFE51A;
    display: flex;
    align-items: center;
    justify-content: center;
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.1);
    position: relative;
    pointer-events: none;
  }

  /* Shadow ellipse offset slightly */
  .joystick-inner::after {
    content: '';
    position: absolute;
    width: 102%;
    height: 102%;
    border-radius: 50%;
    background: #A49714;
    top: -1%;
    left: -0.5%;
    z-index: -1;
    pointer-events: none;
  }

  .joystick-ring {
    position: relative;
    width: 95%;
    aspect-ratio: 1;
    border-radius: 50%;
    background: radial-gradient(circle at 35% 35%, #f5f5f5, #c0c0c0);
    border: 4px solid #ede6e6;
    box-shadow: inset 0 2px 6px rgba(0, 0, 0, 0.15);
    pointer-events: none;
    overflow: hidden;
  }

  /* Cross-hairs */
  .joystick-ring::before,
  .joystick-ring::after {
    content: '';
    position: absolute;
    background: rgba(0, 0, 0, 0.06);
    pointer-events: none;
  }
  .joystick-ring::before {
    width: 1px;
    height: 100%;
    left: 50%;
    top: 0;
    transform: translateX(-50%);
  }
  .joystick-ring::after {
    width: 100%;
    height: 1px;
    top: 50%;
    left: 0;
    transform: translateY(-50%);
  }

  /* ── deadzone ring ─────────────────────── */
  .deadzone-indicator {
    position: absolute;
    /* width = 2 × deadzone fraction × ring size */
    width: calc(var(--dz-pct, 30%) * 2);
    aspect-ratio: 1;
    border-radius: 50%;
    border: 1.5px dashed rgba(0, 0, 0, 0.18);
    background: rgba(0, 0, 0, 0.04);
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    pointer-events: none;
    z-index: 1;
  }

  .joystick-thumb {
    position: absolute;
    width: 48%;
    aspect-ratio: 1;
    border-radius: 50%;
    background: linear-gradient(145deg, #f5f5f5, #c8c8c8);
    border: 2px solid #ede6e6;
    box-shadow:
      0 2px 6px rgba(0, 0, 0, 0.25),
      inset 0 1px 2px rgba(255, 255, 255, 0.6);
    left: 50%;
    top: 50%;
    transform: translate(-50%, -50%);
    transition: box-shadow 0.1s ease;
    will-change: transform;
    pointer-events: none;
  }

  .joystick-thumb::after {
    content: '';
    position: absolute;
    width: 30%;
    aspect-ratio: 1;
    border-radius: 50%;
    background: radial-gradient(circle at 40% 40%, #ddd, #999);
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
  }

  .joystick-zone.active .joystick-thumb {
    box-shadow:
      0 4px 12px rgba(0, 0, 0, 0.35),
      inset 0 1px 2px rgba(255, 255, 255, 0.6);
  }

  /* ════════════════════════════════════════
     D-PAD
     ════════════════════════════════════════ */
  .dpad {
    position: absolute;
    left: 4.05%;
    top: 40.29%;
    width: 38.71%;
    aspect-ratio: 162.588 / 166.554;
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    grid-template-rows: repeat(3, 1fr);
    gap: 2%;
  }

  .dpad-btn {
    width: 100%;
    height: 100%;
    border: none;
    border-radius: 12%;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: all 0.08s ease;
    background: #f9f7f7;
    box-shadow:
      0 2px 0 #c0b000,
      0 3px 6px rgba(0, 0, 0, 0.2);
  }

  .dpad-btn svg {
    pointer-events: none;
    width: 45%;
    height: auto;
  }

  /* Position each D-Pad button in the 3x3 grid */
  .dpad-btn:nth-child(1) {
    grid-column: 2;
    grid-row: 1;
    border-radius: 20% 20% 8% 8%;
  }
  .dpad-btn:nth-child(2) {
    grid-column: 1;
    grid-row: 2;
    border-radius: 20% 8% 8% 20%;
  }
  .dpad-btn:nth-child(3) {
    grid-column: 3;
    grid-row: 2;
    border-radius: 8% 20% 20% 8%;
  }
  .dpad-btn:nth-child(4) {
    grid-column: 2;
    grid-row: 3;
    border-radius: 8% 8% 20% 20%;
  }

  .dpad-btn.pressed,
  .dpad-btn:active {
    transform: scale(0.92);
    box-shadow:
      0 1px 0 #c0b000,
      0 1px 3px rgba(0, 0, 0, 0.2);
    background: #e8e4e4;
  }

  /* ════════════════════════════════════════
     ACTION BUTTONS (Diamond)
     ════════════════════════════════════════ */
  .action-btns {
    position: absolute;
    right: 7.14%;
    top: 40.29%;
    width: 38.57%;
    aspect-ratio: 1;
  }

  .action-btn {
    position: absolute;
    width: 28.6%;
    aspect-ratio: 1;
    border: none;
    border-radius: 25px;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: all 0.08s ease;
    background: #f9f7f7;
    box-shadow:
      0 3px 0 #c0b000,
      0 4px 8px rgba(0, 0, 0, 0.25);
  }

  .action-btn svg {
    pointer-events: none;
    width: 70%;
    height: auto;
  }

  /* Diamond positions */
  .action-btn:nth-child(1) { /* top */
    top: 0;
    left: 50%;
    transform: translateX(-50%);
  }
  .action-btn:nth-child(2) { /* right */
    top: 50%;
    right: 0;
    transform: translateY(-50%);
  }
  .action-btn:nth-child(3) { /* bottom */
    bottom: 0;
    left: 50%;
    transform: translateX(-50%);
  }
  .action-btn:nth-child(4) { /* left */
    top: 50%;
    left: 0;
    transform: translateY(-50%);
  }

  .action-btn.pressed,
  .action-btn:active {
    box-shadow:
      0 1px 0 #c0b000,
      0 2px 4px rgba(0, 0, 0, 0.2);
    background: #e8e4e4;
  }

  .action-btn:nth-child(1).pressed,
  .action-btn:nth-child(1):active {
    transform: translateX(-50%) scale(0.90);
  }
  .action-btn:nth-child(2).pressed,
  .action-btn:nth-child(2):active {
    transform: translateY(-50%) scale(0.90);
  }
  .action-btn:nth-child(3).pressed,
  .action-btn:nth-child(3):active {
    transform: translateX(-50%) scale(0.90);
  }
  .action-btn:nth-child(4).pressed,
  .action-btn:nth-child(4):active {
    transform: translateY(-50%) scale(0.90);
  }

  /* (status indicators and event log removed) */
</style>
