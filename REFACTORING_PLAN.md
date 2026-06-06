# Refactoring Plan

## Goal

Improve stability and reliability of `pc-control` while keeping the tool small and predictable.

## Work Items

1. Move command execution out of the MQTT callback and into the main loop.
   - The callback should only record pending work and return quickly.
   - The main loop should execute Windows power actions.
2. Replace shared `volatile` state with thread-safe synchronization.
   - Use Windows `Interlocked*` APIs or C11 atomics for flags touched by callbacks.
3. Validate command payloads and ignore retained command messages.
   - Avoid accidental sleep/monitor-off after subscribing to retained command topics.
4. Add timeouts and error logging around Windows power APIs.
   - Use `SendMessageTimeout` for monitor-off.
   - Log `SetSuspendState` failures with `GetLastError`.
5. Make status publishing more deterministic.
   - Wait for retained status/version delivery where it matters, especially shutdown.
6. Improve process and logging robustness.
   - Add a single-instance guard per hostname.
   - Move logs to a predictable user-writable location and consider rotation.
7. Tighten input validation and build checks.
   - Validate broker address/port and check `snprintf` truncation.
   - Enable compiler warnings in CMake/CI.

## Progress

- Done: item 1, command execution now happens in the main loop.
- Done: item 2, shared runtime flags now use Windows `Interlocked*` helpers.
- Done: item 3, command messages now require explicit payloads and retained command messages are ignored.
- Current: item 4, add timeouts and error logging around Windows power APIs.
