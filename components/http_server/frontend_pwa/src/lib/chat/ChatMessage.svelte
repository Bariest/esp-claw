<script>
  import { marked } from "marked";

  let { msg, isFirst = false, isLastInGroup = false, respondPermission } = $props();

  /* ── Markdown renderer ─────────────────── */
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

  function fmtTime(ts) {
    if (!ts) return "";
    const d = ts < 100000000000 ? new Date(ts * 1000) : new Date(ts);
    return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  }

  let isUser = $derived(msg.role === "user");
  let isBot = $derived(msg.role === "bot");
  let isSystem = $derived(msg.role === "system");
  let isStep = $derived(msg.role === "step");

  // Timeline helpers for user messages
  let history = $derived(msg.history || []);
  let lastEntry = $derived(history.length > 0 ? history[history.length - 1] : null);
  let hasMultipleEntries = $derived(history.length > 1);
</script>

{#if isStep}
  <!-- Step message -->
  <div class="msg-row msg-row-step">
    <div class="step-bubble">
      <div class="step-header">
        <span class="step-number">{msg.seq}</span>
        <span class="step-label">Step {msg.seq} of {msg.total}</span>
      </div>
      <p class="step-text">{msg.text}</p>
    </div>
  </div>
{:else if isSystem}
  <!-- System message -->
  <div class="msg-row msg-row-center">
    <div class="system-bubble">
      {#if msg.pending && msg.actionId}
        <p>{msg.text}</p>
        <div class="action-btns">
          <button class="approve-btn" onclick={() => respondPermission(msg.actionId, true)}>✅ Approve</button>
          <button class="deny-btn" onclick={() => respondPermission(msg.actionId, false)}>⛔ Deny</button>
        </div>
      {:else}
        <p>{msg.text}</p>
      {/if}
    </div>
  </div>
{:else if isUser}
  <!-- User message -->
  <div class="msg-row msg-row-user">
    <div class="user-wrapper">
      <div class="user-bubble">
        <p class="user-text">{msg.text}</p>
        <div class="user-meta">
          <span class="msg-time">{fmtTime(msg.ts)}</span>
          {#if msg.status === "sending" || msg.status?.startsWith("step_")}
            <span class="sending-dot"></span>
          {:else if msg.status === "completed"}
            <span class="check-icon">✓</span>
          {/if}
        </div>
      </div>
      <!-- Timeline indicator -->
      {#if history.length > 0}
        <div class="timeline">
          {#if !hasMultipleEntries}
            <!-- Single entry — plain status line -->
            <div class="timeline-single">
              {#if lastEntry.stage === "sending"}
                <span class="dot dot-yellow"></span><span>Sending…</span>
              {:else if lastEntry.stage === "sent"}
                <span class="dot dot-blue"></span><span>Sent</span>
              {:else if lastEntry.stage === "relayed"}
                <span class="dot dot-blue"></span><span>Relayed to cloud</span>
              {:else if lastEntry.stage === "processing"}
                <span class="dot dot-purple"></span><span>AI processing…</span>
              {:else if lastEntry.stage === "step"}
                <span class="dot dot-orange"></span><span>Step {lastEntry.seq}/{lastEntry.total}: {lastEntry.text}</span>
              {:else if lastEntry.stage === "completed"}
                <span class="dot dot-green"></span><span class="text-green">✓ Complete</span>
              {/if}
            </div>
          {:else}
            <!-- Multiple entries — expandable -->
            <details class="timeline-details">
              <summary class="timeline-summary">
                {#if lastEntry.stage === "sending"}
                  <span class="dot dot-yellow"></span><span>Sending…</span>
                {:else if lastEntry.stage === "sent"}
                  <span class="dot dot-blue"></span><span>Sent</span>
                {:else if lastEntry.stage === "relayed"}
                  <span class="dot dot-blue"></span><span>Relayed to cloud</span>
                {:else if lastEntry.stage === "processing"}
                  <span class="dot dot-purple"></span><span>AI processing…</span>
                {:else if lastEntry.stage === "step"}
                  <span class="dot dot-orange"></span><span>Step {lastEntry.seq}/{lastEntry.total}: {lastEntry.text}</span>
                {:else if lastEntry.stage === "completed"}
                  <span class="dot dot-green"></span><span class="text-green">✓ Complete</span>
                {/if}
                <span class="timeline-expand">▶ <span class="count">{history.length - 1}</span></span>
              </summary>
              <div class="timeline-entries">
                {#each [...history].reverse().slice(1) as entry}
                  <div class="timeline-entry">
                    {#if entry.stage === "sending"}
                      <span class="dot dot-yellow-dim"></span><span>Sending…</span>
                    {:else if entry.stage === "sent"}
                      <span class="dot dot-blue-dim"></span><span>Sent</span>
                    {:else if entry.stage === "relayed"}
                      <span class="dot dot-blue-dim"></span><span>Relayed to cloud</span>
                    {:else if entry.stage === "processing"}
                      <span class="dot dot-purple-dim"></span><span>AI processing…</span>
                    {:else if entry.stage === "step"}
                      <span class="dot dot-orange-dim"></span><span>Step {entry.seq}/{entry.total}: {entry.text}</span>
                    {:else if entry.stage === "completed"}
                      <span class="dot dot-green-dim"></span><span class="text-green-dim">✓ Complete</span>
                    {/if}
                  </div>
                {/each}
              </div>
            </details>
          {/if}
        </div>
      {/if}
    </div>
  </div>
{:else if isBot}
  <!-- Bot message -->
  <div class="msg-row msg-row-bot">
    <div class="bot-wrapper">
      <div class="bot-bubble">
        <div class="bot-content markdown-body">
          {@html renderMarkdown(msg.text)}
        </div>
        <div class="bot-meta">
          {#if msg.ts}
            <span class="msg-time">{fmtTime(msg.ts)}</span>
          {/if}
          {#if msg.commands?.length}
            <span class="cmd-badge">{msg.commands.length} cmd</span>
          {/if}
        </div>
      </div>
      <!-- Expandable Lua commands -->
      {#if msg.commands?.length}
        <details class="cmd-details">
          <summary class="cmd-summary">
            ▶ {msg.commands.length} Lua {msg.commands.length === 1 ? "command" : "commands"}
          </summary>
          <div class="cmd-list">
            {#each msg.commands as cmd, i}
              <div class="cmd-item">
                <div class="cmd-item-header">
                  <span class="cmd-item-num">#{i + 1}</span>
                  <span class="cmd-item-type">{cmd.type}</span>
                </div>
                <pre class="cmd-item-script">{cmd.script}</pre>
              </div>
            {/each}
          </div>
        </details>
      {/if}
      <!-- Command results -->
      {#if msg.commandResults?.length}
        <div class="cmd-results">
          {#each msg.commandResults as result}
            <div class="cmd-result" class:completed={result.status === 'completed'} class:error={result.status === 'error'} class:timeout={result.status === 'timeout'}>
              <div class="cmd-result-header">
                <span class="cmd-status">
                  {#if result.status === "completed"}✓
                  {:else if result.status === "error"}✗
                  {:else if result.status === "timeout"}⏱
                  {:else}ℹ{/if}
                </span>
                <span class="cmd-status-label">{result.status}</span>
              </div>
              {#if result.output}
                <pre class="cmd-output">{result.output}</pre>
              {/if}
            </div>
          {/each}
        </div>
      {/if}
    </div>
  </div>
{/if}

<style>
  .msg-row {
    display: flex;
    padding: 2px 16px;
  }
  .msg-row-step { justify-content: flex-start; }
  .msg-row-center { justify-content: center; }
  .msg-row-user { justify-content: flex-end; }
  .msg-row-bot { justify-content: flex-start; }

  /* ── Step bubble ─────────────────────── */
  .step-bubble {
    max-width: 85%;
    border-radius: 12px;
    padding: 10px 14px;
    background: #f5f5f5;
    border-left: 3px solid #FFE605;
  }
  .step-header {
    display: flex;
    align-items: center;
    gap: 6px;
    margin-bottom: 4px;
  }
  .step-number {
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: rgba(255, 230, 5, 0.25);
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 0.65rem;
    font-weight: 700;
    color: #b89e00;
  }
  .step-label {
    font-size: 0.7rem;
    color: #999;
  }
  .step-text {
    font-size: 0.8rem;
    color: #555;
  }

  /* ── System bubble ───────────────────── */
  .system-bubble {
    max-width: 90%;
    border-radius: 12px;
    padding: 8px 14px;
    background: #fafafa;
    border: 1px solid #eee;
    text-align: center;
    font-size: 0.75rem;
    color: #999;
    font-style: italic;
  }
  .action-btns {
    display: flex;
    justify-content: center;
    gap: 8px;
    margin-top: 8px;
  }
  .approve-btn {
    padding: 6px 14px;
    border: 1px solid #4ade80;
    border-radius: 8px;
    background: rgba(74, 222, 128, 0.1);
    color: #22c55e;
    font-size: 0.75rem;
    cursor: pointer;
    transition: background 0.12s;
  }
  .approve-btn:hover { background: rgba(74, 222, 128, 0.2); }
  .deny-btn {
    padding: 6px 14px;
    border: 1px solid #f87171;
    border-radius: 8px;
    background: rgba(248, 113, 113, 0.1);
    color: #ef4444;
    font-size: 0.75rem;
    cursor: pointer;
    transition: background 0.12s;
  }
  .deny-btn:hover { background: rgba(248, 113, 113, 0.2); }

  /* ── User bubble ─────────────────────── */
  .user-wrapper {
    display: flex;
    flex-direction: column;
    align-items: flex-end;
    max-width: 80%;
  }
  .user-bubble {
    background: #FFE605;
    border-radius: 16px 16px 4px 16px;
    padding: 10px 14px;
  }
  .user-text {
    font-size: 0.85rem;
    color: #000;
    line-height: 1.4;
    word-wrap: break-word;
  }
  .user-meta {
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 4px;
    margin-top: 4px;
  }
  .msg-time {
    font-size: 0.6rem;
    color: rgba(0, 0, 0, 0.35);
  }
  .sending-dot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: rgba(0, 0, 0, 0.3);
    animation: pulse 1.2s infinite;
  }
  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.3; }
  }
  .check-icon {
    font-size: 0.6rem;
    color: rgba(0, 0, 0, 0.4);
  }

  /* ── Timeline ────────────────────────── */
  .timeline {
    margin-top: 2px;
    align-self: flex-end;
  }
  .timeline-single {
    display: flex;
    align-items: center;
    gap: 4px;
    font-size: 0.68rem;
    color: #999;
  }
  .timeline-details {
    font-size: 0.68rem;
  }
  .timeline-summary {
    display: flex;
    align-items: center;
    gap: 4px;
    color: #999;
    list-style: none;
    cursor: pointer;
  }
  .timeline-summary::-webkit-details-marker { display: none; }
  .timeline-summary::marker { display: none; content: ""; }
  .timeline-expand {
    color: #bbb;
    display: flex;
    align-items: center;
    gap: 2px;
  }
  .timeline-expand .count {
    font-size: 0.62rem;
  }
  .timeline-details[open] .timeline-expand { color: #FFE605; }
  .timeline-entries {
    margin-top: 2px;
    padding-left: 4px;
  }
  .timeline-entry {
    display: flex;
    align-items: center;
    gap: 4px;
    color: #bbb;
  }

  /* ── Dot colors ──────────────────────── */
  .dot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    flex-shrink: 0;
  }
  .dot-yellow { background: #eab308; }
  .dot-blue { background: #3b82f6; }
  .dot-purple { background: #a855f7; }
  .dot-orange { background: #FFE605; }
  .dot-green { background: #22c55e; }
  .dot-yellow-dim { background: rgba(234, 179, 8, 0.5); }
  .dot-blue-dim { background: rgba(59, 130, 246, 0.5); }
  .dot-purple-dim { background: rgba(168, 85, 247, 0.5); }
  .dot-orange-dim { background: rgba(255, 230, 5, 0.5); }
  .dot-green-dim { background: rgba(34, 197, 94, 0.5); }
  .text-green { color: #22c55e; }
  .text-green-dim { color: rgba(34, 197, 94, 0.6); }

  /* ── Bot bubble ──────────────────────── */
  .bot-wrapper {
    max-width: 85%;
  }
  .bot-bubble {
    background: #fff;
    border: 1px solid #e8e8e8;
    border-radius: 16px 16px 16px 4px;
    padding: 12px 14px;
    box-shadow: 0 1px 3px rgba(0,0,0,0.04);
  }
  .bot-content {
    font-size: 0.85rem;
    color: #333;
    line-height: 1.5;
  }
  .bot-meta {
    display: flex;
    align-items: center;
    justify-content: flex-start;
    gap: 8px;
    margin-top: 6px;
  }
  .bot-meta .msg-time {
    color: rgba(0, 0, 0, 0.3);
  }
  .cmd-badge {
    font-size: 0.6rem;
    padding: 2px 6px;
    border-radius: 4px;
    background: #f0f0f0;
    color: #888;
  }

  /* ── Expandable commands ──────────────── */
  .cmd-details {
    margin-top: 2px;
  }
  .cmd-summary {
    font-size: 0.7rem;
    color: #999;
    cursor: pointer;
    list-style: none;
    padding-left: 2px;
  }
  .cmd-summary::-webkit-details-marker { display: none; }
  .cmd-summary::marker { display: none; content: ""; }
  .cmd-summary:hover { color: #FFE605; }
  .cmd-list {
    margin-top: 4px;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .cmd-item {
    border-radius: 6px;
    padding: 6px 8px;
    background: #fafafa;
    border-left: 2px solid rgba(255, 230, 5, 0.5);
  }
  .cmd-item-header {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .cmd-item-num {
    font-size: 0.6rem;
    color: #bbb;
  }
  .cmd-item-type {
    font-size: 0.58rem;
    padding: 1px 5px;
    border-radius: 3px;
    background: rgba(255, 230, 5, 0.2);
    color: #b89e00;
    text-transform: uppercase;
    font-weight: 600;
  }
  .cmd-item-script {
    font-size: 0.68rem;
    color: #555;
    font-family: 'JetBrains Mono', monospace;
    overflow-x: auto;
    white-space: pre-wrap;
    margin-top: 3px;
  }

  /* ── Command results ─────────────────── */
  .cmd-results {
    margin-top: 4px;
  }
  .cmd-result {
    border-radius: 8px;
    padding: 8px 12px;
    margin-bottom: 4px;
    border-left: 3px solid;
  }
  .cmd-result.completed {
    background: #f0fdf4;
    border-color: #22c55e;
  }
  .cmd-result.error {
    background: #fef2f2;
    border-color: #ef4444;
  }
  .cmd-result.timeout {
    background: #fefce8;
    border-color: #eab308;
  }
  .cmd-result-header {
    display: flex;
    align-items: center;
    gap: 6px;
    margin-bottom: 2px;
  }
  .cmd-status {
    font-size: 0.7rem;
    font-weight: 600;
  }
  .cmd-result.completed .cmd-status { color: #22c55e; }
  .cmd-result.error .cmd-status { color: #ef4444; }
  .cmd-result.timeout .cmd-status { color: #eab308; }
  .cmd-status-label {
    font-size: 0.58rem;
    padding: 1px 5px;
    border-radius: 3px;
    text-transform: uppercase;
    font-weight: 600;
  }
  .cmd-result.completed .cmd-status-label {
    background: rgba(34, 197, 94, 0.15);
    color: #22c55e;
  }
  .cmd-result.error .cmd-status-label {
    background: rgba(239, 68, 68, 0.15);
    color: #ef4444;
  }
  .cmd-result.timeout .cmd-status-label {
    background: rgba(234, 179, 8, 0.15);
    color: #eab308;
  }
  .cmd-output {
    font-family: 'JetBrains Mono', monospace;
    font-size: 0.7rem;
    color: #555;
    margin-top: 4px;
    white-space: pre-wrap;
    overflow-x: auto;
  }

  :global(.msg-link) {
    color: #b89e00;
    text-decoration: underline;
  }

  :global(.bot-content.markdown-body p) {
    margin: 0.4em 0;
  }
  :global(.bot-content.markdown-body p:first-child) {
    margin-top: 0;
  }
  :global(.bot-content.markdown-body p:last-child) {
    margin-bottom: 0;
  }
  :global(.bot-content.markdown-body ul),
  :global(.bot-content.markdown-body ol) {
    padding-left: 1.2em;
    margin: 0.3em 0;
  }
  :global(.bot-content.markdown-body code) {
    background: #f0f0f0;
    padding: 1px 4px;
    border-radius: 3px;
    font-size: 0.8em;
  }
  :global(.bot-content.markdown-body pre) {
    background: #f5f5f5;
    padding: 8px 10px;
    border-radius: 6px;
    font-size: 0.75rem;
    overflow-x: auto;
  }
  :global(.bot-content.markdown-body blockquote) {
    border-left: 3px solid #FFE605;
    padding-left: 10px;
    margin: 0.4em 0;
    color: #888;
  }
  :global(.bot-content.markdown-body table) {
    font-size: 0.75rem;
    border-collapse: collapse;
  }
  :global(.bot-content.markdown-body th),
  :global(.bot-content.markdown-body td) {
    border: 1px solid #e8e8e8;
    padding: 4px 8px;
  }
</style>
