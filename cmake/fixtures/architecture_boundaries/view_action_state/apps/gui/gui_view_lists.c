/* Negative fixture for VIEW_ACTION_STATE (A7): a view writing the actions
 * layer's deferred state directly instead of declaring an intent. */
void bad_view_writes_pending_flag(void) {
    s_pending_pack = true;
}
