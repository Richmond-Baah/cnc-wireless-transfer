#pragma once

#ifdef FILEXFER_STACK_AUDIT
// Every 60 s logs each task's stack size and lowest free stack since it started
// ("STACK <who> <task> size=<B> free=<B>", " LOW" under 512 B free). publish is
// optional: it gets "<task> <size> <free>;..." (the receiver's log is UART only).
void stack_audit_start(const char *who, void (*publish)(const char *text));
// Logs one report made by another board.
void stack_audit_log_report(const char *who, char *text);
#endif
