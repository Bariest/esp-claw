# Pointing the robot at a model

The agent runs with no credentials until you give it four values. Until then
every chat answers `LLM API token is not configured`.

## Where

**`http://<robot-ip>/settings`** — ESP-Claw's own settings UI. The PWA at `/`
is the robot interface; `/settings` is the configuration one, and the catch-all
in `http_server_mpx_web.c` routes it to a separate bundle.

Changes apply immediately. `claw_core_agent_0: LLM config updated` in the log
is the confirmation; no reboot.

## Poe

Poe exposes an OpenAI-compatible endpoint, so it needs no special backend.

| Field | Value |
|---|---|
| backend | `openai_compatible` |
| base URL | `https://api.poe.com/v1` |
| API key | from <https://poe.com/api/keys> |
| model | a Poe **bot name**, e.g. `Claude-Sonnet-4.6`, `GPT-5.4`, `Gemini-3.1-Pro` |
| auth type | `bearer` (the default; leave empty) |
| supports tools | **`true`** — see below |
| max tokens | `8192` is fine |

The backend appends `/chat/completions` itself
(`CLAW_LLM_BACKEND_OPENAI_COMPATIBLE_CHAT_PATH`), so the base URL stops at
`/v1`. A trailing slash is handled either way.

Calls draw on your existing Poe subscription points rather than separate
billing, and the documented limit is 500 requests/minute — neither is a
constraint for one robot.

## The one setting that is easy to miss

**`llm_supports_tools` defaults to `false`.** With it off the model can hold a
conversation and nothing else: no `display_show_emotion`, no movement, no
skills. Every capability group registered at boot is invisible to it, and the
failure is silent — the robot just answers in words and never acts.

Poe supports tool calling (`tools` and `tool_choice`), so set it to `true`.
Its `strict` parameter is ignored, so tool arguments are not schema-guaranteed;
the capability handlers validate their own input anyway.

## Same thing over the API

The settings page posts to ESP-Claw's `/api/config`. Flat keys, every value a
string, partial writes applied:

```bash
curl -X POST http://<robot-ip>/api/config \
  -H 'Content-Type: application/json' \
  -d '{
        "llm_backend_type":   "openai_compatible",
        "llm_base_url":       "https://api.poe.com/v1",
        "llm_api_key":        "<your key>",
        "llm_model":          "Claude-Sonnet-4.6",
        "llm_supports_tools": "true"
      }'
```

`GET /api/config` reads the current values back.

## Other providers

- **Anthropic API directly** — backend `anthropic_compatible`, base URL
  `https://api.anthropic.com/v1`. It sends the `anthropic-version` header
  itself.
- **Anything OpenAI-shaped** (OpenRouter, Groq, a local llama.cpp server) —
  backend `openai_compatible` and its base URL. A plain-HTTP local server works
  too; TLS uses the ESP-IDF certificate bundle, so public HTTPS endpoints need
  nothing extra.

## If it still will not answer

The startup line names what is missing:

```
app_claw: Starting root agent backend=(default) base_url=(empty) model=(empty) token=missing
```

All four have to be non-empty. `backend=(default)` means `llm_backend_type` was
never set, which is its own failure even when the key is present.
