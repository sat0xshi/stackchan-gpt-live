# Vendored XiaoZhi boundary

This directory is vendored from
[`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32) tag `v2.2.4`,
commit `e77dedb1309153bb63fed285772962c920c97dd4`.

M5Stack's compatibility changes from `../patches/xiaozhi-esp32.patch` were
applied first. This project then changes the agent transport and its audio integration:

- adds `main/protocols/gpt_live_protocol.{h,cc}`;
- selects `GptLiveProtocol` in `main/application.cc`;
- accepts transport audio before the asynchronous speaking-state UI update;
- keeps default GPT-Live input active during playback and preserves queued audio;
- uses the existing pass-through conversation processor with synchronized buffer access;
- adds audio-path and hosted Terra delegation lifecycle diagnostics.

The StackChan display, avatar, motion, MCP HAL, and phone-avatar WebSocket code
are preserved. The CoreS3 codec outside this vendored tree now duplicates mono
output into both I2S slots and reports diagnostic levels/registers.
`../patches/gpt-live-transport.patch` records the original transport port, before
the subsequent audio/diagnostic changes. Use Git history and
`../../docs/2026-09-13-live-terra-validation.md` for the current changes.
