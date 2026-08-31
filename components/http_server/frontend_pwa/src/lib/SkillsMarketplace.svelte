<script>
  import { colors, skillTypeColor, skillTypeLabel } from "./colors.js";
  import {
    listSkills,
    getSkillManifest,
    listRobotSkills,
    assignSkill,
    removeSkill,
  } from "./marketplaceApi.js";

  let { onNavigate } = $props();

  const YELLOW = colors.mpx.primary;

  // ── Known skill types for filter chips ────────────────────────
  const SKILL_TYPES = [
    { key: "awa",  label: "AISkill" },
    { key: "wasm", label: "MoveSkill" },
    // { key: "type3", label: "Type 3" },
    // { key: "type4", label: "Type 4" },
  ];

  // ── State ────────────────────────────────────────────────────
  let marketplaceSkills = $state([]);
  let robotSkills = $state([]);
  let loading = $state(true);
  let error = $state("");

  // Search & filter
  let searchQuery = $state("");
  let activeFilters = $state([]); // array of skill type keys e.g. ["awa", "wasm"]

  /* Sorting. A store with no sort reads as a pile; a store with a sort reads
     as a catalogue. Kept as a plain <select> rather than a custom dropdown —
     it is one element, it is keyboard- and screen-reader-correct for free, and
     on a phone the OS picker is better than anything hand-rolled. */
  const SORTS = [
    { key: "name",      label: "Name (A–Z)" },
    { key: "name_desc", label: "Name (Z–A)" },
    { key: "price_asc", label: "Price (low to high)" },
    { key: "price_desc",label: "Price (high to low)" },
    { key: "type",      label: "Type" },
    { key: "owned",     label: "Owned first" },
  ];
  let sortBy = $state("name");

  // Detail view
  let detailSkill = $state(null);
  let detailManifest = $state(null);
  let detailLoading = $state(false);

  // Action in flight
  let actionInFlight = $state(null);
  /* Separate from `error`, which replaces the whole list with a Retry panel.
     A failed subscribe must not hide the catalogue you were looking at. */
  let actionError = $state("");

  /* ── Derived: what the list shows ─────────────────────────────
   * Filter and sort in ONE pass over one array. Nothing intermediate is kept
   * in state, so the only copies alive are the source list and the slice on
   * screen — which matters on a device where the whole app shares the
   * browser with a robot's control page. */
  let filteredSkills = $derived.by(() => {
    let skills = marketplaceSkills;

    // Filter by search
    if (searchQuery.trim()) {
      const q = searchQuery.toLowerCase();
      skills = skills.filter(s =>
        (s.title || "").toLowerCase().includes(q) ||
        (s.skill_type || "").toLowerCase().includes(q) ||
        (s.description || "").toLowerCase().includes(q)
      );
    }

    // Filter by type
    if (activeFilters.length > 0) {
      skills = skills.filter(s =>
        activeFilters.includes((s.skill_type || "").toLowerCase())
      );
    }

    // Sort last, on the already-narrowed set.
    const byName = (a, b) => (a.title || "").localeCompare(b.title || "");
    const sorted = [...skills];
    switch (sortBy) {
    case "name_desc":  sorted.sort((a, b) => byName(b, a)); break;
    case "price_asc":  sorted.sort((a, b) => skillPrice(a) - skillPrice(b) || byName(a, b)); break;
    case "price_desc": sorted.sort((a, b) => skillPrice(b) - skillPrice(a) || byName(a, b)); break;
    case "type":       sorted.sort((a, b) =>
                         (a.skill_type || "").localeCompare(b.skill_type || "") || byName(a, b)); break;
    case "owned":      sorted.sort((a, b) =>
                         (isAssigned(b.id) - isAssigned(a.id)) || byName(a, b)); break;
    default:           sorted.sort(byName);
    }
    return sorted;
  });

  let ownedCount = $derived(
    marketplaceSkills.filter((s) => isAssigned(s.id)).length);

  // ── Fetch ────────────────────────────────────────────────────
  async function fetchMarketplace() {
    loading = true;
    error = "";
    try {
      marketplaceSkills = await listSkills();
    } catch (e) {
      error = e.message || "Failed to load marketplace";
      marketplaceSkills = [];
    }
    loading = false;
  }

  async function fetchRobotSkills() {
    try {
      robotSkills = await listRobotSkills();
    } catch { /* ignore */ }
  }

  async function refreshAll() {
    await Promise.all([fetchMarketplace(), fetchRobotSkills()]);
  }

  $effect(() => { refreshAll(); });

  // ── Helpers ──────────────────────────────────────────────────
  function isAssigned(skillId) {
    return robotSkills.some(s => s.skill_id === skillId);
  }

  /**
   * Extract the author/developer name from available data.
   *
   * Priority:
   *   1. `detailManifest.author` (if manifest was fetched and has it)
   *   2. Parse from skill ID: everything before `~` (e.g. "haris_dev" from "haris_dev~amazon"),
   *      with underscores replaced by spaces and title-cased.
   *   3. Fall back to "Unknown".
   */
  function extractAuthor(skill) {
    // 1. Try manifest
    if (detailManifest?.author) return detailManifest.author;

    // 2. Parse from skill ID (developer namespace before ~)
    if (skill?.id) {
      const parts = skill.id.split("~");
      if (parts.length >= 2 && parts[0]) {
        return parts[0]
          .replace(/_/g, " ")
          .replace(/\b\w/g, (c) => c.toUpperCase());
      }
    }

    return "Unknown";
  }

  function toggleFilter(key) {
    if (activeFilters.includes(key)) {
      activeFilters = activeFilters.filter(f => f !== key);
    } else {
      activeFilters = [...activeFilters, key];
    }
  }

  // ── Subscribe / Refund ────────────────────────────────────────
  /* Both of these used to swallow the failure into console.error. The robot
     was answering 405 to every POST and DELETE here — its handler table had
     filled up and the marketplace's write routes never registered — and the
     screen's only tell was that the button went back to how it looked before.
     A write that did not happen has to say so. */
  async function handleSubscribe(skillId) {
    actionInFlight = skillId;
    actionError = "";
    try {
      await assignSkill(skillId);
      // Only refresh robot skills — marketplace listing hasn't changed
      await fetchRobotSkills();
    } catch (e) {
      console.error("Subscribe failed:", e);
      actionError = `Could not subscribe — ${e.message || e}`;
    }
    actionInFlight = null;
  }

  async function handleRefund(skillId) {
    actionInFlight = skillId;
    actionError = "";
    try {
      await removeSkill(skillId);
      // Only refresh robot skills — marketplace listing hasn't changed
      await fetchRobotSkills();
    } catch (e) {
      console.error("Refund failed:", e);
      actionError = `Could not refund — ${e.message || e}`;
    }
    actionInFlight = null;
  }

  // ── Detail ────────────────────────────────────────────────────
  async function openDetail(skill) {
    detailSkill = skill;
    detailManifest = null;
    detailLoading = true;
    try {
      detailManifest = await getSkillManifest(skill.id);
    } catch {
      detailManifest = null;
    }
    detailLoading = false;
  }

  function closeDetail() {
    detailSkill = null;
    detailManifest = null;
  }

  /*
   * TODO: Checkout flow (future)
   * When skills are monetized, each skill will have a `price` field (e.g. { amount: 4.99, currency: "USD" })
   * and a `checkout_url` field pointing to the payment page.
   *
   * The checkout button should:
   *   1. Call an API endpoint to generate a checkout session: POST /v1/marketplace/skills/{id}/checkout
   *   2. Receive a checkout URL from the response
   *   3. Copy the URL to clipboard using: navigator.clipboard.writeText(url)
   *   4. Show a toast/alert: "Checkout link copied! Open it in your browser to complete payment."
   *
   * Example implementation:
   *
   *   async function handleCheckout(skillId) {
   *     try {
   *       const res = await fetch(`/v1/marketplace/skills/${skillId}/checkout`, { method: "POST" });
   *       const data = await res.json();
   *       await navigator.clipboard.writeText(data.checkout_url);
   *       // Show success toast
   *     } catch (e) {
   *       console.error("Checkout failed:", e);
   *     }
   *   }
   *
   * The checkout button should be placed next to the Subscribe button on the detail page
   * and on the marketplace card, styled as a secondary action.
   */

  /**
   * Placeholder: format a price for display.
   * @param {{ amount: number, currency: string }} price
   * @returns {string} e.g. "$4.99"
   */
  function formatPrice(price) {
    if (!price) return null;
    const symbol = price.currency === "USD" ? "$" : price.currency;
    return `${symbol}${price.amount.toFixed(2)}`;
  }

  // ── Demo pricing + fake checkout ─────────────────────────────
  // NOTE: cosmetic only. The API has no price yet, so prices are derived
  // deterministically from the skill id. Replace skillPrice() and confirmPay()
  // with the real pricing + payment integration when ready.
  const DEMO_PRICES = [0.99, 1.99, 9.99];
  function skillPrice(skill) {
    const id = skill?.id || skill?.title || "";
    let h = 0;
    for (let i = 0; i < id.length; i++) h = (h * 31 + id.charCodeAt(i)) >>> 0;
    return DEMO_PRICES[h % DEMO_PRICES.length];
  }
  function priceLabel(skill) { return `$${skillPrice(skill).toFixed(2)}`; }

  // Payment sheet state
  let payingSkill = $state(null);  // skill being "purchased"
  let payStage = $state("form");   // "form" | "processing" | "success"
  let payOrderId = $state("");
  let payMethod = $state("card");

  function openPay(skill) {
    payingSkill = skill;
    payStage = "form";
    payMethod = "card";
  }
  function closePay() {
    payingSkill = null;
    payStage = "form";
  }
  function confirmPay() {
    if (payStage === "processing") return;
    payStage = "processing";
    const skill = payingSkill;
    // Simulate payment processing, then actually subscribe so the robot demo works.
    setTimeout(async () => {
      try {
        await assignSkill(skill.id);
        await fetchRobotSkills();
      } catch (e) {
        console.error("Subscribe failed:", e);
      }
      payOrderId = "#MD-" + Math.floor(100000 + Math.random() * 899999);
      payStage = "success";
    }, 1900);
  }
</script>

<div class="marketplace-root" style="--yellow: {YELLOW}">
  <!-- Header -->
  <header class="mp-header" style="background: {YELLOW}">
    <button class="back-btn" onclick={() => detailSkill ? closeDetail() : onNavigate("back")} aria-label="Back">
      <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <line x1="19" y1="12" x2="5" y2="12"/>
        <polyline points="12 19 5 12 12 5"/>
      </svg>
    </button>
    <h2 class="mp-title">{detailSkill ? detailSkill.title : "Skill Store"}</h2>
    {#if !detailSkill}
      <button class="refresh-btn" onclick={refreshAll} aria-label="Refresh">
        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <polyline points="23 4 23 10 17 10"/>
          <path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/>
        </svg>
      </button>
    {:else}
      <div style="width:28px"></div>
    {/if}
  </header>

  <!-- ═══ DETAIL VIEW ═══ -->
  {#if detailSkill}
    <div class="mp-detail">
      {#if detailLoading}
        <p class="mp-empty">Loading details…</p>
      {:else if detailManifest}
        <!-- Hero -->
        <div class="mp-detail-hero">
          {#if detailManifest.icon_url}
            <img src={detailManifest.icon_url} alt={detailManifest.name} class="mp-detail-icon" />
          {:else}
            <div class="mp-detail-icon-placeholder" style="background: {skillTypeColor(detailSkill.skill_type)}">
              <span>{detailSkill.title?.charAt(0) || "⚡"}</span>
            </div>
          {/if}
          <div class="mp-detail-hero-info">
            <h3 class="mp-detail-name">{detailManifest.name || detailSkill.title}</h3>
            <p class="mp-detail-author">by {extractAuthor(detailSkill)} · v{detailManifest.version || detailSkill.current_version || "1.0"}</p>
            <span
              class="mp-type-badge mp-type-badge-lg"
              style="--badge-color: {skillTypeColor(detailSkill.skill_type)}"
            >
              {skillTypeLabel(detailSkill.skill_type)}
            </span>
          </div>
        </div>

        <!-- Price -->
        <div class="mp-detail-price-row">
          <span class="mp-detail-price-label">Price</span>
          <span class="mp-detail-price-value">{priceLabel(detailSkill)}</span>
        </div>

        <!-- Description -->
        {#if detailManifest.description}
          <p class="mp-detail-desc">{detailManifest.description}</p>
        {/if}

        <!-- Capabilities -->
        {#if detailManifest.capabilities?.length}
          <div class="mp-detail-section">
            <h4 class="mp-detail-section-title">Capabilities</h4>
            <div class="mp-detail-chips">
              {#each detailManifest.capabilities as cap}
                <span class="mp-chip">{cap}</span>
              {/each}
            </div>
          </div>
        {/if}

        <!-- Tags -->
        {#if detailManifest.tags?.length}
          <div class="mp-detail-section">
            <h4 class="mp-detail-section-title">Tags</h4>
            <div class="mp-detail-chips">
              {#each detailManifest.tags as tag}
                <span class="mp-chip mp-chip-tag">{tag}</span>
              {/each}
            </div>
          </div>
        {/if}

        <!-- README -->
        {#if detailManifest.readme}
          <div class="mp-detail-section">
            <h4 class="mp-detail-section-title">README</h4>
            <div class="mp-readme">
              <p class="mp-readme-text">{detailManifest.readme}</p>
            </div>
          </div>
        {/if}

        <!-- Action buttons -->
        <div class="mp-detail-actions">
          {#if isAssigned(detailSkill.id)}
            <button
              class="mp-action-btn mp-action-refund"
              disabled={actionInFlight === detailSkill.id}
              onclick={() => handleRefund(detailSkill.id)}
            >
              {actionInFlight === detailSkill.id ? "Processing…" : "Refund"}
            </button>
          {:else}
            <button
              class="mp-action-btn mp-action-subscribe"
              onclick={() => openPay(detailSkill)}
            >
              Subscribe
            </button>
          {/if}

          <!--
            TODO: Checkout button (future)
            Uncomment when the checkout flow is implemented.
            See the TODO comment in the <script> section for implementation details.

            <button
              class="mp-action-btn mp-action-checkout"
              onclick={() => handleCheckout(detailSkill.id)}
            >
              Checkout
            </button>
          -->
        </div>
      {:else}
        <p class="mp-empty">Could not load skill details.</p>
      {/if}
    </div>

  {:else}
    <!-- ═══ BROWSE VIEW ═══ -->

    <!-- Search bar -->
    <div class="mp-search-wrap">
      <div class="mp-search-box">
        <svg class="mp-search-icon" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="#969494" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <circle cx="11" cy="11" r="8"/>
          <line x1="21" y1="21" x2="16.65" y2="16.65"/>
        </svg>
        <input
          type="text"
          class="mp-search-input"
          placeholder="Search skills…"
          bind:value={searchQuery}
        />
        {#if searchQuery}
          <button class="mp-search-clear" onclick={() => searchQuery = ""} aria-label="Clear search">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
              <line x1="18" y1="6" x2="6" y2="18"/>
              <line x1="6" y1="6" x2="18" y2="18"/>
            </svg>
          </button>
        {/if}
      </div>
    </div>

    <!-- ═══ TOOLBAR ═══════════════════════════════════════════════
         Filter, sort, and a count. The count is the part that makes a
         store read as a store rather than a pile: it says the list is
         complete and that something is deciding what you see. -->
    <div class="mp-toolbar">
      <div class="mp-chiprow">
        <button class="mp-filter-chip" class:active={activeFilters.length === 0}
                onclick={() => (activeFilters = [])}>All</button>
        {#each SKILL_TYPES as type}
          <button
            class="mp-filter-chip"
            class:active={activeFilters.includes(type.key)}
            onclick={() => toggleFilter(type.key)}
          >
            <span class="mp-filter-dot" style="background: {skillTypeColor(type.key)}"></span>
            {type.label}
          </button>
        {/each}
      </div>

      <label class="mp-sort">
        <span class="mp-sort-label">Sort</span>
        <select class="mp-sort-select" bind:value={sortBy}>
          {#each SORTS as o}<option value={o.key}>{o.label}</option>{/each}
        </select>
      </label>
    </div>

    <!-- What the two kinds ARE. One line, once, instead of two badge colours
         nobody can decode. -->
    <p class="mp-legend">
      <b>MoveSkill</b> downloads onto the robot and makes it move ·
      <b>AISkill</b> runs as a behaviour you switch on
    </p>

    {#if actionError}
      <div class="mp-action-error">
        <span>{actionError}</span>
        <button class="mp-action-error-x" onclick={() => actionError = ""}>✕</button>
      </div>
    {/if}

    <!-- Content -->
    <div class="mp-content">
      {#if loading}
        <p class="mp-empty">Loading marketplace…</p>
      {:else if error}
        <div class="mp-error">
          <p>{error}</p>
          <button class="mp-retry-btn" onclick={fetchMarketplace}>Retry</button>
        </div>
      {:else if filteredSkills.length === 0}
        <div class="mp-empty-state">
          {#if searchQuery || activeFilters.length > 0}
            <p class="mp-empty-title">No matches</p>
            <p class="mp-empty-desc">
              Nothing here matches “{searchQuery || SKILL_TYPES.find(t => activeFilters.includes(t.key))?.label}”.
            </p>
            <button class="mp-retry-btn" onclick={() => { searchQuery = ""; activeFilters = []; }}>
              Clear filters
            </button>
          {:else}
            <p class="mp-empty-title">The store is empty</p>
            <p class="mp-empty-desc">No skills have been published yet.</p>
          {/if}
        </div>
      {:else}
        <p class="mp-count">
          <b>{filteredSkills.length}</b>
          {filteredSkills.length === 1 ? "skill" : "skills"}
          {#if filteredSkills.length !== marketplaceSkills.length}of {marketplaceSkills.length}{/if}
          {#if ownedCount}· <b>{ownedCount}</b> owned{/if}
        </p>

        <!-- Rows, not cards. Aligned columns down the page are what makes a
             list scannable, and scannable is most of what "looks legitimate"
             actually means. No images anywhere: the glyph is the first letter
             on the type colour, so there is nothing to fetch and nothing to
             decode. -->
        <ul class="mp-list">
          {#each filteredSkills as skill (skill.id)}
            <li class="mp-row" class:mp-row-owned={isAssigned(skill.id)}>
              <button class="mp-row-main" onclick={() => openDetail(skill)}>
                <span class="mp-glyph" style="background: {skillTypeColor(skill.skill_type)}">
                  {(skill.title || "?").charAt(0).toUpperCase()}
                </span>

                <span class="mp-row-text">
                  <span class="mp-row-title">{skill.title || "Untitled"}</span>
                  <span class="mp-row-sub">
                    {extractAuthor(skill)} · v{skill.current_version || "1.0"}
                  </span>
                  {#if skill.description}
                    <span class="mp-row-desc">{skill.description}</span>
                  {/if}
                </span>

                <span class="mp-row-right">
                  <span class="mp-price">{priceLabel(skill)}</span>
                  <span class="mp-type" style="--badge-color: {skillTypeColor(skill.skill_type)}">
                    {skillTypeLabel(skill.skill_type)}
                  </span>
                </span>
              </button>

              <div class="mp-row-actions">
                {#if isAssigned(skill.id)}
                  <span class="mp-owned">In your library</span>
                  <button
                    class="mp-btn mp-btn-ghost"
                    disabled={actionInFlight === skill.id}
                    onclick={() => handleRefund(skill.id)}
                  >
                    {actionInFlight === skill.id ? "…" : "Refund"}
                  </button>
                {:else}
                  <button class="mp-btn mp-btn-buy" onclick={() => openPay(skill)}>
                    Get for {priceLabel(skill)}
                  </button>
                {/if}
              </div>
            </li>
          {/each}
        </ul>
      {/if}
    </div>
  {/if}

  <!-- ═══ PAYMENT SHEET (cosmetic demo — wire real payment later) ═══ -->
  {#if payingSkill}
    <div class="pay-scrim" onclick={closePay} role="presentation"></div>
    <div class="pay-sheet">
      <div class="pay-grab"></div>

      {#if payStage === "success"}
        <div class="pay-success">
          <div class="pay-check">
            <svg width="38" height="38" viewBox="0 0 24 24" fill="none" stroke="#fff" stroke-width="3" stroke-linecap="round" stroke-linejoin="round">
              <polyline points="4 12.5 10 18 20 6"/>
            </svg>
          </div>
          <h3 class="pay-success-title">Payment successful</h3>
          <p class="pay-success-sub">{payingSkill.title} is now in your library.</p>
          <div class="pay-receipt">
            <div class="pay-receipt-row"><span>Item</span><b>{payingSkill.title}</b></div>
            <div class="pay-receipt-row"><span>Order ID</span><b>{payOrderId}</b></div>
            <div class="pay-receipt-row"><span>Paid</span><b>{priceLabel(payingSkill)}</b></div>
            <div class="pay-receipt-row"><span>Status</span><b class="pay-ok">Installed ✓</b></div>
          </div>
          <button class="pay-done" onclick={closePay}>Start using it</button>
        </div>
      {:else}
        <h3 class="pay-title">Checkout</h3>
        <p class="pay-sub">Complete your purchase to unlock this skill.</p>

        <div class="pay-order">
          <div class="pay-order-icon" style="background: {skillTypeColor(payingSkill.skill_type)}">
            {payingSkill.title?.charAt(0) || "⚡"}
          </div>
          <div class="pay-order-info">
            <span class="pay-order-name">{payingSkill.title}</span>
            <span class="pay-order-type">{skillTypeLabel(payingSkill.skill_type)}</span>
          </div>
          <span class="pay-order-amt">{priceLabel(payingSkill)}</span>
        </div>

        <div class="pay-methods">
          <button class="pay-method" class:active={payMethod === "card"} onclick={() => payMethod = "card"}>Card</button>
          <button class="pay-method" class:active={payMethod === "apple"} onclick={() => payMethod = "apple"}>Apple Pay</button>
          <button class="pay-method" class:active={payMethod === "gpay"} onclick={() => payMethod = "gpay"}>Google Pay</button>
        </div>

        {#if payMethod === "card"}
          <div class="pay-field">
            <label for="pay-cc">Card number</label>
            <input id="pay-cc" type="text" value="4242 4242 4242 4242" inputmode="numeric" />
          </div>
          <div class="pay-field-row">
            <div class="pay-field">
              <label for="pay-exp">Expiry</label>
              <input id="pay-exp" type="text" value="09/28" />
            </div>
            <div class="pay-field">
              <label for="pay-cvc">CVC</label>
              <input id="pay-cvc" type="text" value="123" inputmode="numeric" />
            </div>
          </div>
          <div class="pay-field">
            <label for="pay-name">Name on card</label>
            <input id="pay-name" type="text" value="Mang Dang" />
          </div>
        {:else}
          <div class="pay-wallet">
            <span class="pay-wallet-label">{payMethod === "apple" ? "Apple Pay" : "Google Pay"} selected</span>
            <span class="pay-wallet-hint">Confirm with a single tap below.</span>
          </div>
        {/if}

        <button class="pay-confirm" disabled={payStage === "processing"} onclick={confirmPay}>
          {#if payStage === "processing"}
            <span class="pay-spinner"></span> Processing…
          {:else}
            Pay {priceLabel(payingSkill)}
          {/if}
        </button>
        <!-- Was "🔒 Secured payment · 256-bit encryption". Padlock emoji and
             a bit-count are what a phishing page says; a real store states
             plainly who takes the money and what happens next. -->
        <p class="pay-secure">Payment is handled by the MPX marketplace. You can
          refund a skill from your library at any time.</p>
      {/if}
    </div>
  {/if}
</div>

<style>
  /* ═══ Store list ═══════════════════════════════════════════════
     Rows with columns that line up, not cards floating in a grid.
     Everything below is plain CSS on flexbox — no library, no images, no
     web fonts. The heaviest thing on this screen is the text. */

  .mp-toolbar {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 0 14px 8px;
  }
  .mp-chiprow {
    display: flex;
    gap: 6px;
    overflow-x: auto;
    scrollbar-width: none;
    flex: 1;
    min-width: 0;
  }
  .mp-chiprow::-webkit-scrollbar { display: none; }

  .mp-sort { display: flex; align-items: center; gap: 5px; flex: none; }
  .mp-sort-label { font-size: 0.68rem; color: #8a8a8a; }
  .mp-sort-select {
    font: inherit;
    font-size: 0.72rem;
    padding: 5px 6px;
    border: 1px solid #dcdcdc;
    border-radius: 8px;
    background: #fff;
    color: #000;
    max-width: 118px;
  }

  .mp-legend {
    margin: 0 14px 8px;
    font-size: 0.68rem;
    line-height: 1.45;
    color: #8a8a8a;
  }
  .mp-legend b { color: #4a4a4a; font-weight: 600; }

  .mp-count {
    margin: 0 2px 8px;
    font-size: 0.72rem;
    color: #8a8a8a;
  }
  .mp-count b { color: #000; font-weight: 700; }

  .mp-list {
    list-style: none;
    margin: 0;
    padding: 0;
    border-top: 1px solid #ececec;
  }

  .mp-row {
    border-bottom: 1px solid #ececec;
    background: #fff;
  }
  /* Owned rows get a rail rather than a fill: it marks them without making
     the list look striped and busy. */
  .mp-row-owned { box-shadow: inset 3px 0 0 var(--yellow); }

  .mp-row-main {
    display: flex;
    align-items: flex-start;
    gap: 11px;
    width: 100%;
    padding: 12px 14px 8px;
    background: none;
    border: 0;
    text-align: left;
    cursor: pointer;
    font: inherit;
  }

  .mp-glyph {
    flex: none;
    width: 38px;
    height: 38px;
    border-radius: 9px;
    display: flex;
    align-items: center;
    justify-content: center;
    color: #fff;
    font-size: 1.05rem;
    font-weight: 800;
  }

  .mp-row-text { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
  .mp-row-title {
    font-size: 0.9rem;
    font-weight: 700;
    color: #000;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .mp-row-sub { font-size: 0.7rem; color: #8a8a8a; }
  .mp-row-desc {
    font-size: 0.72rem;
    line-height: 1.4;
    color: #6a6a6a;
    display: -webkit-box;
    -webkit-line-clamp: 2;
    line-clamp: 2;
    -webkit-box-orient: vertical;
    overflow: hidden;
    margin-top: 2px;
  }

  .mp-row-right {
    flex: none;
    display: flex;
    flex-direction: column;
    align-items: flex-end;
    gap: 5px;
  }
  /* Tabular figures so prices form a column you can compare down, which is
     the single cheapest thing that makes a list of prices look priced. */
  .mp-price {
    font-size: 0.85rem;
    font-weight: 700;
    color: #000;
    font-variant-numeric: tabular-nums;
  }
  .mp-type {
    font-size: 0.6rem;
    font-weight: 700;
    letter-spacing: 0.02em;
    padding: 2px 6px;
    border-radius: 999px;
    color: var(--badge-color);
    border: 1px solid var(--badge-color);
    white-space: nowrap;
  }

  .mp-row-actions {
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 10px;
    padding: 0 14px 11px;
  }
  .mp-owned {
    margin-right: auto;
    font-size: 0.7rem;
    font-weight: 600;
    color: #4a7a4a;
  }
  .mp-btn {
    font: inherit;
    font-size: 0.75rem;
    font-weight: 700;
    padding: 7px 14px;
    border-radius: 9px;
    border: 1px solid transparent;
    cursor: pointer;
  }
  .mp-btn:disabled { opacity: 0.5; cursor: default; }
  .mp-btn-buy { background: var(--yellow); color: #000; }
  .mp-btn-ghost { background: #fff; border-color: #d7d7d7; color: #6a6a6a; }

  .marketplace-root {
    position: absolute;
    inset: 0;
    background: #fff;
    display: flex;
    flex-direction: column;
  }

  /* ── Header ────────────────────────────── */
  .mp-header {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 14px;
    flex-shrink: 0;
    z-index: 2;
  }

  .back-btn {
    display: flex;
    align-items: center;
    justify-content: center;
    background: none;
    border: none;
    cursor: pointer;
    color: #000;
    padding: 4px;
    border-radius: 8px;
  }
  .back-btn:active { background: rgba(0,0,0,0.08); }

  .mp-title {
    flex: 1;
    font-size: 1.05rem;
    font-weight: 700;
    color: #000;
    text-align: center;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .refresh-btn {
    display: flex;
    align-items: center;
    justify-content: center;
    background: none;
    border: none;
    cursor: pointer;
    color: #000;
    padding: 4px;
    border-radius: 8px;
  }
  .refresh-btn:active { background: rgba(0,0,0,0.08); }

  /* ── Search ────────────────────────────── */
  .mp-search-wrap {
    padding: 10px 14px;
    flex-shrink: 0;
  }

  .mp-search-box {
    display: flex;
    align-items: center;
    gap: 8px;
    background: #f0f0f0;
    border-radius: 12px;
    padding: 8px 12px;
  }

  .mp-search-icon {
    flex-shrink: 0;
  }

  .mp-search-input {
    flex: 1;
    border: none;
    background: transparent;
    font-size: 0.9rem;
    color: #000;
    outline: none;
  }
  .mp-search-input::placeholder { color: #969494; }

  .mp-search-clear {
    display: flex;
    align-items: center;
    justify-content: center;
    background: none;
    border: none;
    cursor: pointer;
    color: #969494;
    padding: 2px;
    border-radius: 50%;
  }
  .mp-search-clear:active { color: #000; }

  /* ── Filters ───────────────────────────── */
  .mp-filters {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 0 14px 10px;
    flex-shrink: 0;
    overflow-x: auto;
    -ms-overflow-style: none;
    scrollbar-width: none;
  }
  .mp-filters::-webkit-scrollbar { display: none; }

  .mp-filter-chip {
    display: flex;
    align-items: center;
    gap: 5px;
    padding: 6px 12px;
    border-radius: 999px;
    border: 1.5px solid #e0e0e0;
    background: #fff;
    font-size: 0.78rem;
    font-weight: 500;
    color: #555;
    cursor: pointer;
    white-space: nowrap;
    transition: all 0.15s;
  }
  .mp-filter-chip.active {
    border-color: var(--chip-color);
    background: color-mix(in srgb, var(--chip-color) 10%, #fff);
    color: var(--chip-color);
    font-weight: 600;
  }
  .mp-filter-chip:active { filter: brightness(0.96); }

  .mp-filter-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    flex-shrink: 0;
  }

  .mp-filter-clear {
    background: none;
    border: none;
    font-size: 0.75rem;
    color: #969494;
    cursor: pointer;
    padding: 4px 6px;
    white-space: nowrap;
  }
  .mp-filter-clear:active { color: #000; }

  /* ── Content ───────────────────────────── */
  .mp-content {
    flex: 1;
    overflow-y: auto;
    padding: 0 14px 16px;
  }

  .mp-empty {
    text-align: center;
    color: #969494;
    font-size: 0.9rem;
    margin-top: 40px;
  }

  .mp-action-error {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    margin: 0 16px 8px;
    padding: 10px 12px;
    border-radius: 10px;
    background: #FDECEC;
    border: 1px solid #E9A8A8;
    color: #8C2020;
    font-size: 13px;
    line-height: 1.35;
  }
  .mp-action-error-x {
    flex: none;
    border: 0;
    background: transparent;
    color: inherit;
    font-size: 14px;
    cursor: pointer;
    padding: 0 2px;
  }

  .mp-error {
    text-align: center;
    color: #ED7676;
    font-size: 0.85rem;
    margin-top: 40px;
  }

  .mp-retry-btn {
    margin-top: 10px;
    padding: 6px 16px;
    border-radius: 8px;
    border: 1px solid #ccc;
    background: #fff;
    font-size: 0.8rem;
    cursor: pointer;
    color: #000;
  }
  .mp-retry-btn:active { background: #f5f5f5; }

  /* ── Empty state ───────────────────────── */
  .mp-empty-state {
    display: flex;
    flex-direction: column;
    align-items: center;
    margin-top: 48px;
    text-align: center;
    gap: 8px;
  }
  .mp-empty-icon { font-size: 3rem; }
  .mp-empty-title { font-size: 1rem; font-weight: 600; color: #000; }
  .mp-empty-desc { font-size: 0.8rem; color: #969494; max-width: 240px; line-height: 1.4; }

  /* ── Grid ──────────────────────────────── */
  .mp-grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 10px;
    padding-bottom: 12px;
  }

  /* ── Card ──────────────────────────────── */
  .mp-card {
    background: #f9f9f9;
    border: 1px solid #eee;
    border-radius: 14px;
    overflow: hidden;
    display: flex;
    flex-direction: column;
  }

  .mp-card-main {
    flex: 1;
    display: flex;
    flex-direction: column;
    padding: 12px;
    gap: 6px;
    background: none;
    border: none;
    cursor: pointer;
    text-align: left;
    width: 100%;
  }

  .mp-card-top {
    display: flex;
    align-items: flex-start;
    justify-content: space-between;
    gap: 6px;
  }

  .mp-card-icon {
    width: 36px;
    height: 36px;
    border-radius: 10px;
    display: flex;
    align-items: center;
    justify-content: center;
    font-weight: 700;
    font-size: 1rem;
    color: #fff;
    flex-shrink: 0;
  }

  .mp-card-meta {
    display: flex;
    flex-direction: column;
    align-items: flex-end;
    gap: 3px;
  }

  .mp-type-badge {
    display: inline-block;
    padding: 2px 7px;
    border-radius: 999px;
    font-size: 0.6rem;
    font-weight: 700;
    letter-spacing: 0.03em;
    color: #fff;
    background: var(--badge-color);
    white-space: nowrap;
  }

  .mp-card-version {
    font-size: 0.65rem;
    color: #969494;
  }

  .mp-card-title {
    font-size: 0.85rem;
    font-weight: 600;
    color: #000;
    line-height: 1.3;
    display: -webkit-box;
    -webkit-line-clamp: 2;
    -webkit-box-orient: vertical;
    overflow: hidden;
  }

  .mp-card-author {
    font-size: 0.68rem;
    color: #969494;
  }

  .mp-card-desc {
    font-size: 0.72rem;
    color: #888;
    line-height: 1.35;
    display: -webkit-box;
    -webkit-line-clamp: 2;
    -webkit-box-orient: vertical;
    overflow: hidden;
  }

  /* Price tag (future) */
  .mp-card-price {
    margin-top: auto;
    padding-top: 2px;
  }
  .mp-card-price-value {
    font-size: 0.85rem;
    font-weight: 700;
    color: #000;
  }

  /* ── Card actions ──────────────────────── */
  .mp-card-actions {
    display: flex;
    gap: 6px;
    padding: 8px 12px 12px;
    border-top: 1px solid #eee;
  }

  .mp-card-btn {
    flex: 1;
    padding: 7px 0;
    border-radius: 8px;
    border: none;
    font-size: 0.75rem;
    font-weight: 600;
    cursor: pointer;
    text-align: center;
    transition: opacity 0.15s;
  }
  .mp-card-btn:disabled { opacity: 0.5; cursor: default; }

  .mp-card-btn-sub {
    background: var(--yellow);
    color: #000;
  }
  .mp-card-btn-sub:active:not(:disabled) { filter: brightness(0.92); }

  .mp-card-btn-refund {
    background: #ED7676;
    color: #fff;
  }
  .mp-card-btn-refund:active:not(:disabled) { filter: brightness(0.9); }

  /* Checkout button (future) */
  .mp-card-btn-checkout {
    background: #000;
    color: #fff;
  }

  /* ═══ DETAIL VIEW ═══ */
  .mp-detail {
    flex: 1;
    overflow-y: auto;
    padding: 16px 14px;
    display: flex;
    flex-direction: column;
    gap: 16px;
  }

  .mp-detail-hero {
    display: flex;
    align-items: center;
    gap: 14px;
  }

  .mp-detail-icon {
    width: 64px;
    height: 64px;
    border-radius: 16px;
    object-fit: cover;
  }

  .mp-detail-icon-placeholder {
    width: 64px;
    height: 64px;
    border-radius: 16px;
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 1.6rem;
    font-weight: 700;
    color: #fff;
  }

  .mp-detail-hero-info {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }

  .mp-detail-name {
    font-size: 1.15rem;
    font-weight: 700;
    color: #000;
  }

  .mp-detail-author {
    font-size: 0.78rem;
    color: #969494;
  }

  .mp-type-badge-lg {
    font-size: 0.7rem;
    padding: 3px 10px;
    align-self: flex-start;
  }

  /* Price row (future) */
  .mp-detail-price-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 12px 14px;
    background: #f5f5f5;
    border-radius: 12px;
  }
  .mp-detail-price-label {
    font-size: 0.85rem;
    color: #555;
    font-weight: 500;
  }
  .mp-detail-price-value {
    font-size: 1.1rem;
    font-weight: 700;
    color: #000;
  }

  .mp-detail-desc {
    font-size: 0.85rem;
    color: #444;
    line-height: 1.5;
  }

  .mp-detail-section {
    display: flex;
    flex-direction: column;
    gap: 8px;
  }

  .mp-detail-section-title {
    font-size: 0.72rem;
    font-weight: 700;
    color: #969494;
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }

  .mp-detail-chips {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
  }

  .mp-chip {
    padding: 5px 12px;
    border-radius: 999px;
    font-size: 0.75rem;
    font-weight: 500;
    background: rgba(255, 230, 5, 0.15);
    color: #8a7c00;
  }

  .mp-chip-tag {
    background: #f0f0f0;
    color: #666;
  }

  .mp-readme {
    background: #f5f5f5;
    border-radius: 12px;
    padding: 14px;
  }

  .mp-readme-text {
    font-size: 0.8rem;
    color: #555;
    line-height: 1.6;
    white-space: pre-wrap;
  }

  /* ── Detail actions ────────────────────── */
  .mp-detail-actions {
    display: flex;
    gap: 10px;
    padding-top: 4px;
  }

  .mp-action-btn {
    flex: 1;
    padding: 12px 0;
    border-radius: 12px;
    border: none;
    font-size: 0.9rem;
    font-weight: 600;
    cursor: pointer;
    transition: opacity 0.15s;
    text-align: center;
  }
  .mp-action-btn:disabled { opacity: 0.5; cursor: default; }

  .mp-action-subscribe {
    background: var(--yellow);
    color: #000;
  }
  .mp-action-subscribe:active:not(:disabled) { filter: brightness(0.92); }

  .mp-action-refund {
    background: #ED7676;
    color: #fff;
  }
  .mp-action-refund:active:not(:disabled) { filter: brightness(0.9); }

  /* Checkout button (future) */
  .mp-action-checkout {
    background: #000;
    color: #fff;
  }

  /* ═══ PAYMENT SHEET (demo) ═══ */
  .pay-scrim {
    position: absolute;
    inset: 0;
    background: rgba(0, 0, 0, 0.45);
    z-index: 10;
    animation: pay-fade 0.2s ease;
  }
  @keyframes pay-fade { from { opacity: 0; } to { opacity: 1; } }

  .pay-sheet {
    position: absolute;
    left: 0; right: 0; bottom: 0;
    z-index: 11;
    background: #fff;
    border-radius: 22px 22px 0 0;
    padding: 8px 20px 22px;
    max-height: 92%;
    overflow-y: auto;
    animation: pay-slide 0.32s cubic-bezier(.22, 1, .36, 1);
    box-shadow: 0 -8px 30px rgba(0, 0, 0, 0.18);
  }
  @keyframes pay-slide { from { transform: translateY(100%); } to { transform: translateY(0); } }

  .pay-grab { width: 40px; height: 5px; border-radius: 3px; background: #dcdcdc; margin: 6px auto 14px; }

  .pay-title { font-size: 1.15rem; font-weight: 700; color: #000; }
  .pay-sub { font-size: 0.8rem; color: #969494; margin: 2px 0 16px; }

  .pay-order {
    display: flex;
    align-items: center;
    gap: 12px;
    background: #f7f7f8;
    border-radius: 14px;
    padding: 12px;
    margin-bottom: 16px;
  }
  .pay-order-icon {
    width: 42px; height: 42px;
    border-radius: 11px;
    flex-shrink: 0;
    display: flex; align-items: center; justify-content: center;
    color: #fff; font-weight: 700; font-size: 1.1rem;
  }
  .pay-order-info { display: flex; flex-direction: column; gap: 2px; flex: 1; min-width: 0; }
  .pay-order-name { font-size: 0.9rem; font-weight: 700; color: #000; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .pay-order-type { font-size: 0.72rem; color: #969494; }
  .pay-order-amt { font-size: 1.05rem; font-weight: 800; color: #000; }

  .pay-methods { display: flex; gap: 8px; margin-bottom: 14px; }
  .pay-method {
    flex: 1;
    padding: 10px 4px;
    border-radius: 10px;
    border: 2px solid #ececec;
    background: #fff;
    font-size: 0.75rem;
    font-weight: 600;
    color: #666;
    cursor: pointer;
    white-space: nowrap;
  }
  .pay-method.active { border-color: #000; color: #000; background: #fafafa; }

  .pay-field { margin-bottom: 12px; flex: 1; }
  .pay-field label { display: block; font-size: 0.72rem; font-weight: 600; color: #555; margin-bottom: 5px; }
  .pay-field input {
    width: 100%;
    box-sizing: border-box;
    border: 1.5px solid #e4e4e4;
    border-radius: 10px;
    padding: 11px 12px;
    font-size: 0.9rem;
    outline: none;
    color: #000;
    letter-spacing: 0.02em;
  }
  .pay-field input:focus { border-color: var(--yellow); }
  .pay-field-row { display: flex; gap: 12px; }

  .pay-wallet {
    display: flex;
    flex-direction: column;
    gap: 4px;
    align-items: center;
    background: #f7f7f8;
    border-radius: 12px;
    padding: 20px;
    margin-bottom: 4px;
  }
  .pay-wallet-label { font-size: 0.9rem; font-weight: 700; color: #000; }
  .pay-wallet-hint { font-size: 0.75rem; color: #969494; }

  .pay-confirm {
    width: 100%;
    margin-top: 8px;
    padding: 14px;
    border-radius: 12px;
    border: none;
    background: #000;
    color: #fff;
    font-size: 0.95rem;
    font-weight: 700;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
  }
  .pay-confirm:disabled { opacity: 0.9; cursor: default; }

  .pay-spinner {
    width: 16px; height: 16px;
    border-radius: 50%;
    border: 2.5px solid rgba(255, 255, 255, 0.35);
    border-top-color: #fff;
    animation: pay-spin 0.7s linear infinite;
  }
  @keyframes pay-spin { to { transform: rotate(360deg); } }

  .pay-secure { text-align: center; font-size: 0.7rem; color: #969494; margin-top: 12px; }

  /* success */
  .pay-success { text-align: center; padding: 10px 0 4px; }
  .pay-check {
    width: 74px; height: 74px;
    border-radius: 50%;
    background: #28c76f;
    margin: 8px auto 16px;
    display: flex; align-items: center; justify-content: center;
    animation: pay-pop 0.4s cubic-bezier(.18, 1.5, .4, 1);
  }
  @keyframes pay-pop { from { transform: scale(0); } to { transform: scale(1); } }
  .pay-success-title { font-size: 1.15rem; font-weight: 700; color: #000; }
  .pay-success-sub { font-size: 0.82rem; color: #969494; margin: 6px 0 0; }
  .pay-receipt { background: #f7f7f8; border-radius: 12px; padding: 12px 14px; margin: 16px 0 6px; text-align: left; }
  .pay-receipt-row { display: flex; justify-content: space-between; font-size: 0.8rem; color: #555; padding: 4px 0; }
  .pay-receipt-row b { color: #000; font-weight: 700; }
  .pay-ok { color: #28c76f !important; }
  .pay-done {
    width: 100%;
    margin-top: 14px;
    padding: 14px;
    border-radius: 12px;
    border: none;
    background: var(--yellow);
    color: #000;
    font-size: 0.95rem;
    font-weight: 700;
    cursor: pointer;
  }
</style>
