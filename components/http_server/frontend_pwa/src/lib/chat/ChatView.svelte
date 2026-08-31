<script>
  import { onMount } from "svelte";
  import { marked } from "marked";
  import ChatSidebar from "./ChatSidebar.svelte";
  import ChatMessage from "./ChatMessage.svelte";
  import EmptyChat from "./EmptyChat.svelte";
  import {
    saveConversation,
    getConversation,
    generateUUID,
  } from "../conversationStore.js";

  let { navigate } = $props();

  // ── UUID helper (matches original) ─────────
  function makeUUID() {
    if (typeof crypto !== "undefined" && typeof crypto.randomUUID === "function") {
      return crypto.randomUUID();
    }
    return "10000000-1000-4000-8000-100000000000".replace(/[018]/g, (c) =>
      (c ^ (crypto.getRandomValues(new Uint8Array(1))[0] & (15 >> (c / 4)))).toString(16)
    );
  }

  // ── Session ID persistence ─────────────────
  const SESSION_STORAGE_KEY = "mpx_active_session_id";

  function restoreSessionId() {
    try {
      const saved = localStorage.getItem(SESSION_STORAGE_KEY);
      if (saved) return saved;
    } catch {}
    return makeUUID();
  }

  // ── State ──────────────────────────────────
  let ws = null;
  let reconnectTimer = null;   // single-flight reconnect guard (prevents socket storms)
  let connected = $state(false);
  let sending = $state(false);
  let sessionId = $state(restoreSessionId());
  let messages = $state([]);
  let inputText = $state("");
  let showSidebar = $state(false);
  let sidebarRefreshKey = $state(0);
  let sessionAckBadge = $state(false);
  let scrollAnchor = $state(null);
  let inputEl = $state(null);
  let pendingActions = $state([]);

  // Live timer for response counter
  let now = $state(Date.now());
  let lastSentTs = $state(null);
  $effect(() => {
    const id = setInterval(() => { now = Date.now(); }, 200);
    return () => clearInterval(id);
  });

  // Accumulator for chunked chat_reply reassembly
  let pendingChunks = {};

  // ── Markdown ───────────────────────────────
  const renderer = new marked.Renderer();
  renderer.link = ({ href, text }) =>
    `<a href="${href}" target="_blank" rel="noopener noreferrer" class="msg-link">${text}</a>`;
  marked.setOptions({ renderer, breaks: true, gfm: true });

  function renderMarkdown(text) {
    if (!text) return "";
    const raw = marked.parse(text, { async: false });
    return raw
      .replace(/<script[\s\S]*?<\/script>/gi, "")
      .replace(/\son\w+\s*=\s*["']?[^"'\s>]+["']?/gi, "");
  }

  // ── Timestamp helpers ─────────────────────
  function normalizeTs(ts) { return ts < 100000000000 ? ts * 1000 : ts; }

  function formatTimestamp(ts) {
    if (!ts) return "";
    const d = new Date(normalizeTs(ts));
    return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  }

  function getMsgTs(msg) {
    if (msg.ts) return msg.ts;
    if (msg.history && msg.history.length > 0) return msg.history[0].ts;
    return null;
  }

  function isNewDay(a, b) {
    const tsA = getMsgTs(a);
    const tsB = getMsgTs(b);
    if (!tsA || !tsB) return false;
    const dA = new Date(normalizeTs(tsA));
    const dB = new Date(normalizeTs(tsB));
    return dA.getDate() !== dB.getDate() || dA.getMonth() !== dB.getMonth() || dA.getFullYear() !== dB.getFullYear();
  }

  function findPrevTimestampMsg(msgs, fromIdx) {
    for (let i = fromIdx; i >= 0; i--) {
      if (getMsgTs(msgs[i])) return msgs[i];
    }
    return msgs[fromIdx];
  }

  function formatDateLabel(msg) {
    const ts = getMsgTs(msg);
    if (!ts) return "";
    const d = new Date(normalizeTs(ts));
    const now = new Date();
    const today = new Date(now.getFullYear(), now.getMonth(), now.getDate());
    const dateDay = new Date(d.getFullYear(), d.getMonth(), d.getDate());
    const diffDays = Math.round((today - dateDay) / 86400000);
    if (diffDays === 0) return "Today";
    if (diffDays === 1) return "Yesterday";
    if (diffDays < 7) {
      const days = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"];
      return days[d.getDay()];
    }
    return d.toLocaleDateString([], { month: "short", day: "numeric", year: "numeric" });
  }

  // ── Auto-save ──────────────────────────────
  let debounceTimer;
  function persist() {
    clearTimeout(debounceTimer);
    debounceTimer = setTimeout(() => {
      if (messages.length > 0) {
        saveConversation(sessionId, { messages });
        sidebarRefreshKey++;
      }
    }, 500);
  }

  $effect(() => {
    const msgs = messages;
    if (msgs.length > 0) persist();
  });

  $effect(() => {
    try { localStorage.setItem(SESSION_STORAGE_KEY, sessionId); } catch {}
  });

  // ── Conversation actions ──────────────────
  function selectConversation(id) {
    const convo = getConversation(id);
    if (!convo) return;
    if (messages.length > 0) saveConversation(sessionId, { messages });
    sidebarRefreshKey++;
    sessionId = convo.id;
    messages = convo.messages || [];
    sending = false;
    if (window.innerWidth < 768) showSidebar = false;
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        type: "session_reset",
        session_id: sessionId,
        ts: Math.floor(Date.now() / 1000),
      }));
    }
  }

  function newConversation() {
    if (messages.length > 0) saveConversation(sessionId, { messages });
    sidebarRefreshKey++;
    const newId = makeUUID();
    sessionId = newId;
    messages = [];
    sending = false;
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        type: "session_reset",
        session_id: newId,
        ts: Math.floor(Date.now() / 1000),
      }));
    }
    inputEl?.focus();
  }

  function handleDelete(id) {
    if (id === sessionId) {
      const newId = makeUUID();
      sessionId = newId;
      messages = [];
      sending = false;
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({
          type: "session_reset",
          session_id: newId,
          ts: Math.floor(Date.now() / 1000),
        }));
      }
    }
  }

  // ── Permission response ────────────────────
  function respondPermission(actionId, approved) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify({
      type: "permission_response",
      action_id: actionId,
      approved,
      ts: Math.floor(Date.now() / 1000),
    }));
    pendingActions = pendingActions.filter((a) => a.id !== actionId);
    // Update the matching system message
    for (let i = messages.length - 1; i >= 0; i--) {
      if (messages[i].actionId === actionId) {
        const updated = { ...messages[i] };
        updated.pending = false;
        const prefix = approved ? "✅ Approved: " : "⛔ Denied: ";
        updated.text = prefix + messages[i].text.replace("🔧 OpenClaw requests: ", "");
        messages[i] = updated;
        break;
      }
    }
  }

  // ═══════════════════════════════════════════
  //  WebSocket — ported from working Chat.svelte
  // ═══════════════════════════════════════════
  const WS_URL = `ws://${location.host}/v1/chat/ui`;

  function scheduleReconnect() {
    // Single-flight: never stack more than one pending reconnect.
    if (reconnectTimer) return;
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      if (document.visibilityState !== "hidden") connect();
    }, 3000);
  }

  function connect() {
    // Don't open a second socket if one is already open OR still connecting.
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;

    // Make sure any previous socket is fully torn down before opening a new one.
    if (ws) {
      ws.onopen = ws.onclose = ws.onerror = ws.onmessage = null;
      try { ws.close(); } catch {}
      ws = null;
    }

    const socket = new WebSocket(WS_URL);
    ws = socket;   // claim the slot immediately so re-entrant calls are guarded

    socket.onopen = () => {
      if (socket !== ws) return;        // stale socket — ignore
      connected = true;
      socket.send(JSON.stringify({
        type: "session_reset",
        session_id: sessionId,
        ts: Math.floor(Date.now() / 1000),
        pwa_build: "stormfix-v14",
      }));
    };

    socket.onclose = () => {
      if (socket !== ws) return;        // a stale/replaced socket closing — do NOT reconnect
      connected = false;
      ws = null;
      scheduleReconnect();
    };

    socket.onerror = () => {
      if (socket === ws) connected = false;
    };

    socket.onmessage = (event) => {
      try {
        const data = JSON.parse(event.data);
        handleMessage(data);
      } catch (e) {
        console.warn("chat WS: unparseable frame dropped", e, event.data);
      }
    };
  }

  function disconnect() {
    if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
    if (ws) {
      ws.onopen = ws.onclose = ws.onerror = ws.onmessage = null;
      try { ws.close(); } catch {}
      ws = null;
    }
    connected = false;
  }

  function handleMessage(data) {
    // ── Chunked chat_reply reassembly (array-based like original) ──
    if (data.type === "chat_reply_chunk") {
      const sid = data.session_id;
      if (!pendingChunks[sid]) {
        pendingChunks[sid] = {
          parts: new Array(data.total),
          total: data.total,
          count: 0,
        };
      }
      const acc = pendingChunks[sid];
      if (acc.parts[data.seq] === undefined) {
        acc.parts[data.seq] = data.text;
        acc.count++;
      }
      if (acc.count < acc.total) return;
      const fullText = acc.parts.join("");
      delete pendingChunks[sid];
      handleMessage({ type: "chat_reply", text: fullText, commands: [], session_id: sid });
      return;
    }

    // ── openclaw_action ──
    if (data.type === "openclaw_action") {
      const action = {
        id: data.action_id || "",
        type: data.action_type || "unknown",
        description: data.description || "",
        status: data.status || "completed",
      };
      if (action.status === "pending" && action.id) {
        pendingActions = [...pendingActions, action];
      } else if (action.id) {
        pendingActions = pendingActions.filter((a) => a.id !== action.id);
        const icon = action.status === "approved" ? "✅" : action.status === "denied" ? "⛔" : action.status === "timeout" ? "⏱️" : "ℹ️";
        messages = [...messages, {
          role: "system",
          text: `${icon} OpenClaw action: ${action.type} — ${action.description} [${action.status}]`,
        }];
      }
      if (action.status === "pending") {
        messages = [...messages, {
          role: "system",
          text: `🔧 OpenClaw requests: ${action.description}`,
          actionId: action.id,
          actionType: action.type,
          pending: true,
        }];
      }
      sending = false;
      return;
    }

    // ── step ──
    if (data.type === "step") {
      // Update the most recent user message's history
      const lastIdx = messages.length - 1;
      if (lastIdx >= 0 && messages[lastIdx].role === "user") {
        const updated = { ...messages[lastIdx] };
        if (!updated.history) updated.history = [];
        updated.status = `step_${data.seq}_of_${data.total}`;
        updated.history = [
          ...updated.history,
          { stage: "step", ts: data.ts, text: data.text, seq: data.seq, total: data.total },
        ];
        messages[lastIdx] = updated;
      }
      // Also append as a visible step message
      messages = [...messages, {
        role: "step",
        text: data.text,
        seq: data.seq,
        total: data.total,
        ts: data.ts,
      }];
      return;
    }

    // ── command_result ──
    if (data.type === "command_result") {
      for (let i = messages.length - 1; i >= 0; i--) {
        if (messages[i].role === "bot") {
          const updated = { ...messages[i] };
          if (!updated.commandResults) updated.commandResults = [];
          updated.commandResults = [...updated.commandResults, {
            script: data.script,
            output: data.output,
            status: data.status,
          }];
          messages[i] = updated;
          break;
        }
      }
      return;
    }

    // ── chat_reply ──
    if (data.type === "chat_reply") {
      if (data.text && data.text.includes("Session reset acknowledged")) {
        sessionAckBadge = true;
        setTimeout(() => { sessionAckBadge = false; }, 3000);
        sending = false;
        return;
      }
      // Mark last user message as completed
      const lastIdx = messages.length - 1;
      if (lastIdx >= 0 && messages[lastIdx].role === "user") {
        const updated = { ...messages[lastIdx] };
        updated.status = "completed";
        if (!updated.history) updated.history = [];
        updated.history = [...updated.history, { stage: "completed", ts: data.ts }];
        updated.reply = data;
        messages[lastIdx] = updated;
      }
      // Dedupe — skip if last bot message has identical text
      const lastMsg = messages.length > 0 ? messages[messages.length - 1] : null;
      const newMsg = {
        role: "bot",
        text: data.text,
        commands: data.commands || [],
        ts: data.ts,
      };
      if (lastMsg && lastMsg.role === "bot" && lastMsg.text === data.text) {
        messages = [...messages.slice(0, -1), newMsg];
      } else {
        messages = [...messages, newMsg];
      }
      sending = false;
      return;
    }

    // ── ack ──
    if (data.type === "ack") {
      const lastIdx = messages.length - 1;
      if (lastIdx >= 0 && messages[lastIdx].role === "user") {
        const updated = { ...messages[lastIdx] };
        updated.status = data.status || "sent";
        if (!updated.history) updated.history = [];
        updated.history = [...updated.history, { stage: data.status, ts: Date.now() }];
        messages[lastIdx] = updated;
      }
      return;
    }

    // ── error ──
    if (data.type === "error" && data.text) {
      messages = [...messages, { role: "system", text: `⚠️ ${data.text}` }];
      sending = false;
      return;
    }
  }

  // ── Send message ──────────────────────────
  function sendMessage(text) {
    if (!text.trim() || sending || !ws || ws.readyState !== WebSocket.OPEN) return;

    const nowTs = Date.now();
    messages = [...messages, {
      role: "user",
      text: text.trim(),
      status: "sending",
      history: [{ stage: "sending", ts: nowTs }],
      ts: nowTs,
    }];
    inputText = "";
    sending = true;
    lastSentTs = nowTs;

    ws.send(JSON.stringify({
      type: "user_chat_input",
      text: text.trim(),
      session_id: sessionId,
      ts: Math.floor(nowTs / 1000),
    }));
  }

  // ── Lifecycle ──────────────────────────────
  onMount(() => {
    const convo = getConversation(sessionId);
    if (convo && convo.messages.length > 0) {
      messages = convo.messages;
    }
    connect();
    return () => disconnect();
  });

  // ── Auto-scroll ────────────────────────────
  $effect(() => {
    if (messages.length > 0 && scrollAnchor) {
      requestAnimationFrame(() => {
        scrollAnchor.scrollIntoView({ behavior: "smooth", block: "end" });
      });
    }
  });

  // ── Auto-focus input ──────────────────────
  $effect(() => {
    if (!sending && inputEl && connected) {
      inputEl.focus();
    }
  });
</script>

<div class="chat-root">
  <!-- Sidebar (slide animation) -->
  <div class="sidebar-backdrop" class:visible={showSidebar} onclick={() => (showSidebar = false)}></div>
  <div class="sidebar-panel" class:open={showSidebar}>
    <ChatSidebar
      activeId={sessionId}
      refreshKey={sidebarRefreshKey}
      onSelect={selectConversation}
      onNew={newConversation}
      onDelete={handleDelete}
    />
  </div>

  <!-- Main chat area -->
  <div class="chat-main">
    <!-- Action bar -->
    <div class="action-bar">
      <button
        class="action-bar-btn"
        class:active={showSidebar}
        onclick={() => (showSidebar = !showSidebar)}
        aria-label="Conversation history"
        title="Conversation history"
      >
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <circle cx="12" cy="12" r="10"/>
          <polyline points="12 6 12 12 16 14"/>
        </svg>
        <span>History</span>
      </button>
      <button
        class="action-bar-btn"
        onclick={newConversation}
        aria-label="New conversation"
        title="New conversation"
      >
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <line x1="12" y1="5" x2="12" y2="19"/>
          <line x1="5" y1="12" x2="19" y2="12"/>
        </svg>
        <span>New</span>
      </button>
      <span class="flex items-center gap-1.5 text-xs ml-auto">
        {#if sessionAckBadge}
          <span class="flex items-center gap-1 text-mpx-orange-light">
            <span class="w-2 h-2 rounded-full bg-mpx-orange-light"></span>
            Session reset
          </span>
        {:else}
          <span class="flex items-center gap-1.5 {connected ? 'text-green-500' : 'text-red-500'}">
            <span class="w-2 h-2 rounded-full {connected ? 'bg-green-500' : 'bg-red-500'}"></span>
            {sending ? "Sending…" : connected ? "Online" : "Offline"}
          </span>
        {/if}
      </span>
    </div>

    <!-- Messages or Empty state -->
    {#if messages.length === 0 && !sending}
      <EmptyChat />
    {:else}
      <div class="messages-area">
        <div class="messages-scroll">
          {#each messages as msg, i}
            <!-- Date delineator -->
            {#if i === 0 || (i > 0 && isNewDay(findPrevTimestampMsg(messages, i - 1), msg))}
              <div class="date-delineator">
                <span>{formatDateLabel(msg)}</span>
              </div>
            {/if}
            <ChatMessage
              {msg}
              isFirst={i === 0}
              isLastInGroup={i === messages.length - 1 || messages[i + 1]?.role !== msg.role}
              {respondPermission}
            />
          {/each}

          <!-- Sending indicator -->
          {#if sending && lastSentTs}
            <div class="sending-indicator">
              <span class="spinner"></span>
              <span class="timer">{Math.floor((now - lastSentTs) / 1000)}s</span>
            </div>
          {/if}

          <div bind:this={scrollAnchor}></div>
        </div>
      </div>
    {/if}

    <!-- Input bar -->
    <div class="input-bar">
      <div class="input-row">
        <input
          bind:this={inputEl}
          bind:value={inputText}
          onkeydown={(e) => e.key === "Enter" && sendMessage(inputText)}
          placeholder={connected ? "Type a message…" : "Reconnecting…"}
          disabled={sending || !connected}
          class="chat-input"
        />
        <button
          class="send-btn"
          onclick={() => sendMessage(inputText)}
          disabled={!inputText.trim() || sending || !connected}
        >
          {#if sending}
            <span class="spinner small"></span>
          {:else}
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
              <line x1="22" y1="2" x2="11" y2="13"/>
              <polygon points="22 2 15 22 11 13 2 9 22 2"/>
            </svg>
          {/if}
        </button>
      </div>
    </div>
  </div>
</div>

<style>
  .chat-root {
    display: flex;
    height: 100%;
    background: #f8f8f8;
    position: relative;
  }

  /* ── Sidebar (slide) ──────────────────── */
  .sidebar-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0,0,0,0.3);
    z-index: 40;
    opacity: 0;
    pointer-events: none;
    transition: opacity 0.3s ease;
  }
  .sidebar-backdrop.visible {
    opacity: 1;
    pointer-events: auto;
  }

  .sidebar-panel {
    position: fixed;
    top: 0;
    left: 0;
    bottom: 0;
    width: 280px;
    z-index: 50;
    transform: translateX(-100%);
    transition: transform 0.3s cubic-bezier(0.22, 1, 0.36, 1);
    box-shadow: 4px 0 12px rgba(0,0,0,0.08);
  }
  .sidebar-panel.open {
    transform: translateX(0);
  }

  /* ── Main chat area ──────────────────── */
  .chat-main {
    flex: 1;
    display: flex;
    flex-direction: column;
    min-width: 0;
  }

  /* ── Action bar ───────────────────────── */
  .action-bar {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 8px 14px;
    border-bottom: 1px solid #eee;
    background: #f8f8f8;
    flex-shrink: 0;
  }

  .action-bar-btn {
    display: flex;
    align-items: center;
    gap: 5px;
    padding: 6px 12px;
    border: 1px solid #e0e0e0;
    border-radius: 8px;
    background: #fff;
    color: #666;
    font-size: 0.75rem;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.15s;
  }
  .action-bar-btn:hover {
    border-color: #FFE605;
    color: #333;
    background: #fffef5;
  }
  .action-bar-btn.active {
    border-color: #FFE605;
    background: #fffde0;
    color: #333;
  }

  /* ── Date delineator ──────────────────── */
  .date-delineator {
    display: flex;
    justify-content: center;
    margin: 10px 0;
  }
  .date-delineator span {
    font-size: 0.68rem;
    color: #999;
    background: #f0f0f0;
    padding: 3px 12px;
    border-radius: 100px;
    border: 1px solid #e0e0e0;
  }

  /* ── Messages ─────────────────────────── */
  .messages-area {
    flex: 1;
    overflow-y: auto;
    min-height: 0;
  }

  .messages-scroll {
    padding: 8px 0;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .sending-indicator {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 16px;
    margin: 4px 16px;
    border-radius: 12px;
    background: #f5f5f5;
    border: 1px solid #eee;
    width: fit-content;
  }

  .spinner {
    width: 18px;
    height: 18px;
    border: 2.5px solid rgba(255, 230, 5, 0.4);
    border-top-color: #FFE605;
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }
  .spinner.small {
    width: 16px;
    height: 16px;
    border-width: 2px;
  }
  @keyframes spin {
    to { transform: rotate(360deg); }
  }

  .timer {
    font-size: 0.8rem;
    font-weight: 600;
    color: #999;
    font-variant-numeric: tabular-nums;
  }

  /* ── Input bar ───────────────────────── */
  .input-bar {
    padding: 10px 14px 16px;
    border-top: 1px solid #eee;
    background: #f8f8f8;
    flex-shrink: 0;
  }

  .input-row {
    display: flex;
    align-items: center;
    gap: 8px;
    background: #f0f0f0;
    border-radius: 100px;
    padding: 4px 4px 4px 18px;
  }

  .chat-input {
    flex: 1;
    border: none;
    outline: none;
    background: transparent;
    font-size: 0.9rem;
    color: #333;
    padding: 10px 0;
    min-width: 0;
  }
  .chat-input::placeholder {
    color: #bbb;
  }
  .chat-input:disabled {
    opacity: 0.5;
  }

  .send-btn {
    width: 40px;
    height: 40px;
    border-radius: 50%;
    border: none;
    background: #222;
    color: #fff;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: background 0.15s;
    flex-shrink: 0;
  }
  .send-btn:hover {
    background: #444;
  }
  .send-btn:disabled {
    opacity: 0.4;
    cursor: default;
  }
</style>
