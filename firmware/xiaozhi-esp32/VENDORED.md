# Vendored XiaoZhi boundary

This directory is vendored from
[`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32) tag `v2.2.4`,
commit `e77dedb1309153bb63fed285772962c920c97dd4`.

M5Stack's compatibility changes from `../patches/xiaozhi-esp32.patch` were
applied first. This project then changes only the agent transport boundary:

- adds `main/protocols/gpt_live_protocol.{h,cc}`;
- selects `GptLiveProtocol` in `main/application.cc`;
- accepts transport audio before the asynchronous speaking-state UI update.

The StackChan display, avatar, motion, CoreS3 codec, MCP HAL, and phone-avatar
WebSocket code are unchanged. To review the GPT-Live boundary against the
M5Stack-compatible XiaoZhi tree, see `../patches/gpt-live-transport.patch`.
