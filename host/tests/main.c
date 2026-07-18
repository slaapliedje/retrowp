#include "test.h"

/* endian.c */
void test_endian_read_write16(void);
void test_endian_read_write32(void);
void test_endian_roundtrip_values(void);
void test_endian_swap(void);
void test_endian_host_is_little_endian_consistent(void);

/* utf8.c */
void test_utf8_ascii_roundtrip(void);
void test_utf8_2byte_roundtrip(void);
void test_utf8_3byte_roundtrip(void);
void test_utf8_4byte_roundtrip(void);
void test_utf8_rejects_surrogate(void);
void test_utf8_rejects_overlong(void);
void test_utf8_rejects_truncated(void);
void test_utf8_seq_len_table(void);
void test_utf8_next_prev_and_count(void);
void test_utf8_validate_good_and_bad(void);

/* platform/host */
void test_mem_leak_counters(void);
void test_file_roundtrip(void);
void test_pipe_transport_loopback_and_timeout(void);
void test_fake_monospace_metrics(void);

/* cursor.c */
void test_cursor_forward_crosses_paragraph_boundary(void);
void test_cursor_forward_backward_step_counts_match(void);
void test_cursor_next_prev_are_true_inverses(void);

/* edit.c */
void test_edit_insert_at_start_of_paragraph(void);
void test_edit_insert_at_end_of_paragraph(void);
void test_edit_insert_mid_run_splits_correctly(void);
void test_edit_insert_into_empty_paragraph(void);
void test_edit_insert_style_inheritance_at_boundary(void);
void test_edit_delete_whole_run(void);
void test_edit_delete_left_partial(void);
void test_edit_delete_right_partial(void);
void test_edit_delete_mid_run_splits(void);
void test_edit_delete_rejects_invalid_range(void);
void test_edit_insert_then_delete_restores_content(void);

/* undo.c */
void test_undo_insert_then_undo_then_redo(void);
void test_undo_delete_all_cases_then_undo_redo(void);
void test_undo_new_edit_clears_redo(void);
void test_undo_empty_journal_returns_error(void);
void test_undo_no_leaks(void);
void test_undo_fuzz_n_ops_then_n_undos_restores_original(void);

/* strpool.c */
void test_strpool_reserves_offset_zero(void);
void test_strpool_multiple_appends_survive_growth(void);
void test_strpool_empty_append(void);

/* docmodel.c */
void test_docmodel_init_free_has_no_leaks(void);
void test_docmodel_init_sets_usable_defaults(void);
void test_docmodel_cstyle_add_survives_growth(void);
void test_docmodel_build_known_tree(void);
void test_docmodel_pstyle_and_cstyle_set(void);

/* format_txt.c */
void test_format_txt_roundtrip_simple(void);
void test_format_txt_roundtrip_empty(void);
void test_format_txt_roundtrip_multi_para(void);
void test_format_txt_roundtrip_blank_lines(void);
void test_format_txt_roundtrip_unicode(void);
void test_format_txt_import_no_trailing_newline(void);
void test_format_txt_import_crlf(void);
void test_format_txt_import_empty_creates_zero_paragraphs(void);
void test_format_txt_export_reports_size_on_overflow(void);

/* offload/transport/wire.c */
void test_wire_crc16_known_vector(void);
void test_wire_encode_decode_roundtrip(void);
void test_wire_rejects_corrupt_frame_and_resyncs(void);
void test_wire_timeout_on_empty_stream(void);
void test_wire_payload_too_large_for_buffer(void);

/* offload_client.c + offload/gateway (mock) */
void test_offload_available_false_without_device(void);
void test_offload_thesaurus_happy_path(void);
void test_offload_thesaurus_unknown_word_returns_empty_list(void);
void test_offload_spellcheck_flags_known_misspelling(void);
void test_offload_spellcheck_correct_word(void);
void test_offload_unsupported_op_reports_unsupported(void);
void test_offload_corrupt_response_is_rejected(void);
void test_offload_timeout_on_delayed_gateway(void);
void test_offload_gateway_dies_before_responding(void);
void test_offload_short_frame_times_out(void);
void test_offload_submit_fails_after_gateway_killed(void);

static const test_case g_cases[] = {
    { "endian_read_write16", test_endian_read_write16 },
    { "endian_read_write32", test_endian_read_write32 },
    { "endian_roundtrip_values", test_endian_roundtrip_values },
    { "endian_swap", test_endian_swap },
    { "endian_host_is_little_endian_consistent", test_endian_host_is_little_endian_consistent },

    { "utf8_ascii_roundtrip", test_utf8_ascii_roundtrip },
    { "utf8_2byte_roundtrip", test_utf8_2byte_roundtrip },
    { "utf8_3byte_roundtrip", test_utf8_3byte_roundtrip },
    { "utf8_4byte_roundtrip", test_utf8_4byte_roundtrip },
    { "utf8_rejects_surrogate", test_utf8_rejects_surrogate },
    { "utf8_rejects_overlong", test_utf8_rejects_overlong },
    { "utf8_rejects_truncated", test_utf8_rejects_truncated },
    { "utf8_seq_len_table", test_utf8_seq_len_table },
    { "utf8_next_prev_and_count", test_utf8_next_prev_and_count },
    { "utf8_validate_good_and_bad", test_utf8_validate_good_and_bad },

    { "mem_leak_counters", test_mem_leak_counters },
    { "file_roundtrip", test_file_roundtrip },
    { "pipe_transport_loopback_and_timeout", test_pipe_transport_loopback_and_timeout },
    { "fake_monospace_metrics", test_fake_monospace_metrics },

    { "cursor_forward_crosses_paragraph_boundary", test_cursor_forward_crosses_paragraph_boundary },
    { "cursor_forward_backward_step_counts_match", test_cursor_forward_backward_step_counts_match },
    { "cursor_next_prev_are_true_inverses", test_cursor_next_prev_are_true_inverses },

    { "edit_insert_at_start_of_paragraph", test_edit_insert_at_start_of_paragraph },
    { "edit_insert_at_end_of_paragraph", test_edit_insert_at_end_of_paragraph },
    { "edit_insert_mid_run_splits_correctly", test_edit_insert_mid_run_splits_correctly },
    { "edit_insert_into_empty_paragraph", test_edit_insert_into_empty_paragraph },
    { "edit_insert_style_inheritance_at_boundary", test_edit_insert_style_inheritance_at_boundary },
    { "edit_delete_whole_run", test_edit_delete_whole_run },
    { "edit_delete_left_partial", test_edit_delete_left_partial },
    { "edit_delete_right_partial", test_edit_delete_right_partial },
    { "edit_delete_mid_run_splits", test_edit_delete_mid_run_splits },
    { "edit_delete_rejects_invalid_range", test_edit_delete_rejects_invalid_range },
    { "edit_insert_then_delete_restores_content", test_edit_insert_then_delete_restores_content },

    { "undo_insert_then_undo_then_redo", test_undo_insert_then_undo_then_redo },
    { "undo_delete_all_cases_then_undo_redo", test_undo_delete_all_cases_then_undo_redo },
    { "undo_new_edit_clears_redo", test_undo_new_edit_clears_redo },
    { "undo_empty_journal_returns_error", test_undo_empty_journal_returns_error },
    { "undo_no_leaks", test_undo_no_leaks },
    { "undo_fuzz_n_ops_then_n_undos_restores_original", test_undo_fuzz_n_ops_then_n_undos_restores_original },

    { "strpool_reserves_offset_zero", test_strpool_reserves_offset_zero },
    { "strpool_multiple_appends_survive_growth", test_strpool_multiple_appends_survive_growth },
    { "strpool_empty_append", test_strpool_empty_append },

    { "docmodel_init_free_has_no_leaks", test_docmodel_init_free_has_no_leaks },
    { "docmodel_init_sets_usable_defaults", test_docmodel_init_sets_usable_defaults },
    { "docmodel_cstyle_add_survives_growth", test_docmodel_cstyle_add_survives_growth },
    { "docmodel_build_known_tree", test_docmodel_build_known_tree },
    { "docmodel_pstyle_and_cstyle_set", test_docmodel_pstyle_and_cstyle_set },

    { "format_txt_roundtrip_simple", test_format_txt_roundtrip_simple },
    { "format_txt_roundtrip_empty", test_format_txt_roundtrip_empty },
    { "format_txt_roundtrip_multi_para", test_format_txt_roundtrip_multi_para },
    { "format_txt_roundtrip_blank_lines", test_format_txt_roundtrip_blank_lines },
    { "format_txt_roundtrip_unicode", test_format_txt_roundtrip_unicode },
    { "format_txt_import_no_trailing_newline", test_format_txt_import_no_trailing_newline },
    { "format_txt_import_crlf", test_format_txt_import_crlf },
    { "format_txt_import_empty_creates_zero_paragraphs", test_format_txt_import_empty_creates_zero_paragraphs },
    { "format_txt_export_reports_size_on_overflow", test_format_txt_export_reports_size_on_overflow },

    { "wire_crc16_known_vector", test_wire_crc16_known_vector },
    { "wire_encode_decode_roundtrip", test_wire_encode_decode_roundtrip },
    { "wire_rejects_corrupt_frame_and_resyncs", test_wire_rejects_corrupt_frame_and_resyncs },
    { "wire_timeout_on_empty_stream", test_wire_timeout_on_empty_stream },
    { "wire_payload_too_large_for_buffer", test_wire_payload_too_large_for_buffer },

    { "offload_available_false_without_device", test_offload_available_false_without_device },
    { "offload_thesaurus_happy_path", test_offload_thesaurus_happy_path },
    { "offload_thesaurus_unknown_word_returns_empty_list", test_offload_thesaurus_unknown_word_returns_empty_list },
    { "offload_spellcheck_flags_known_misspelling", test_offload_spellcheck_flags_known_misspelling },
    { "offload_spellcheck_correct_word", test_offload_spellcheck_correct_word },
    { "offload_unsupported_op_reports_unsupported", test_offload_unsupported_op_reports_unsupported },
    { "offload_corrupt_response_is_rejected", test_offload_corrupt_response_is_rejected },
    { "offload_timeout_on_delayed_gateway", test_offload_timeout_on_delayed_gateway },
    { "offload_gateway_dies_before_responding", test_offload_gateway_dies_before_responding },
    { "offload_short_frame_times_out", test_offload_short_frame_times_out },
    { "offload_submit_fails_after_gateway_killed", test_offload_submit_fails_after_gateway_killed }
};

int main(void)
{
    int count = (int)(sizeof(g_cases) / sizeof(g_cases[0]));
    return test_run_all(g_cases, count);
}
