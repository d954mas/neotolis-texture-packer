#include "tp_cancel_source.h"

bool tp_cancel_source_poll(void *context) {
    tp_cancel_source *source = (tp_cancel_source *)context;
    if (!source) {
        return false;
    }
    /* Latched: answer without touching the channel. A second destructive read
     * after the first cancel would consume an unrelated byte (or an EOF) and
     * could only ever confirm what is already known. */
    if (source->observed != TP_CANCEL_REASON_NONE) {
        return true;
    }
    if (!source->probe) {
        return false;
    }
    const tp_cancel_reason reason = source->probe(source->ctx);
    if (reason != TP_CANCEL_REASON_NONE) {
        source->observed = reason;
    }
    return source->observed != TP_CANCEL_REASON_NONE;
}

tp_cancel_token tp_cancel_source_token(tp_cancel_source *source) {
    const tp_cancel_token token = {tp_cancel_source_poll, source};
    return token;
}
