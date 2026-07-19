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

/* linebreak.c */
void test_layout_greedy_wrap_hand_computed_fixture(void);
void test_layout_single_word_wider_than_column_overflows(void);
void test_layout_empty_paragraph_gets_one_zero_width_line(void);
void test_layout_word_spanning_run_boundary_is_one_token(void);
void test_layout_editing_one_paragraph_does_not_remeasure_whole_document(void);

/* paginate.c */
void test_paginate_known_page_size_yields_expected_page_count(void);
void test_paginate_new_section_always_starts_new_page(void);
void test_paginate_no_leaks(void);

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

/* format_rtf_import.c */
void test_format_rtf_import_rejects_bad_header(void);
void test_format_rtf_import_rejects_unsupported_charset(void);
void test_format_rtf_import_simple_paragraph(void);
void test_format_rtf_import_no_trailing_empty_paragraph(void);
void test_format_rtf_import_blank_line_is_empty_paragraph(void);
void test_format_rtf_import_bold_italic_underline(void);
void test_format_rtf_import_font_size(void);
void test_format_rtf_import_alignment_and_indents(void);
void test_format_rtf_import_color_table(void);
void test_format_rtf_import_hex_escape_decodes_cp1252(void);
void test_format_rtf_import_special_chars(void);
void test_format_rtf_import_skips_fonttbl_and_ignorable_destination(void);
void test_format_rtf_import_unrecognized_control_word_is_ignored(void);
void test_format_rtf_import_truncated_hex_escape_is_badfmt(void);
void test_format_rtf_import_exceeds_max_nesting_depth_is_range(void);
void test_format_rtf_import_truncated_input_recovers_leniently(void);
void test_format_rtf_import_no_leaks(void);

/* format_rtf_export.c */
void test_format_rtf_export_reports_size_on_overflow(void);
void test_format_rtf_roundtrip_plain_text(void);
void test_format_rtf_roundtrip_full_attribute_set(void);

/* spell.c */
void test_spell_dict_open_missing_file_fails(void);
void test_spell_check_exact_known_word(void);
void test_spell_check_unknown_word(void);
void test_spell_check_case_insensitive(void);
void test_spell_check_affix_rules(void);
void test_spell_check_non_ascii_fails_open(void);
void test_spell_check_no_leaks(void);
void test_spell_suggest_finds_transposition(void);
void test_spell_suggest_respects_max(void);
void test_spell_suggest_non_ascii_returns_nothing(void);
void test_spell_realistic_dictionary_sample(void);

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
void test_offload_grammar_check_known_fixture(void);
void test_offload_grammar_check_clean_paragraph_returns_no_issues(void);
void test_offload_chat_happy_path(void);
void test_offload_chat_unrecognized_prompt_gets_generic_reply(void);
void test_offload_capabilities_advertise_chat_but_not_image(void);
void test_offload_unsupported_op_reports_unsupported(void);
void test_offload_corrupt_response_is_rejected(void);
void test_offload_timeout_on_delayed_gateway(void);
void test_offload_gateway_dies_before_responding(void);
void test_offload_short_frame_times_out(void);
void test_offload_submit_fails_after_gateway_killed(void);

/* offload/gateway/odt_gateway.py (M7 T7.2) */
void test_convert_odt_import_via_gateway(void);
void test_convert_odt_export_roundtrip_via_gateway(void);
void test_convert_pdf_export_via_gateway(void);
void test_convert_unsupported_format_id_reports_unsupported(void);
void test_convert_malformed_payload_is_error(void);

/* services/grammar/grammar.c (M8 T8.2) */
void test_grammar_doubled_word_flags_whole_phrase_and_suggests_one_copy(void);
void test_grammar_doubled_word_is_case_insensitive(void);
void test_grammar_no_doubled_word_for_different_words(void);
void test_grammar_triple_word_reports_one_issue_and_moves_past_it(void);
void test_grammar_multiple_spaces_suggests_single_space(void);
void test_grammar_single_space_is_not_flagged(void);
void test_grammar_space_before_punctuation_includes_the_mark(void);
void test_grammar_no_space_before_punctuation_is_not_flagged(void);
void test_grammar_repeated_exclamation_flagged(void);
void test_grammar_ellipsis_is_not_flagged_as_repeated_punctuation(void);
void test_grammar_missing_initial_capital_flagged(void);
void test_grammar_capitalized_start_is_not_flagged(void);
void test_grammar_clean_text_reports_no_issues(void);
void test_grammar_empty_text_reports_no_issues(void);
void test_grammar_max_issues_cap_is_respected(void);
void test_grammar_realistic_paragraph_finds_every_planted_issue(void);

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

    { "layout_greedy_wrap_hand_computed_fixture", test_layout_greedy_wrap_hand_computed_fixture },
    { "layout_single_word_wider_than_column_overflows", test_layout_single_word_wider_than_column_overflows },
    { "layout_empty_paragraph_gets_one_zero_width_line", test_layout_empty_paragraph_gets_one_zero_width_line },
    { "layout_word_spanning_run_boundary_is_one_token", test_layout_word_spanning_run_boundary_is_one_token },
    { "layout_editing_one_paragraph_does_not_remeasure_whole_document", test_layout_editing_one_paragraph_does_not_remeasure_whole_document },

    { "paginate_known_page_size_yields_expected_page_count", test_paginate_known_page_size_yields_expected_page_count },
    { "paginate_new_section_always_starts_new_page", test_paginate_new_section_always_starts_new_page },
    { "paginate_no_leaks", test_paginate_no_leaks },

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

    { "format_rtf_import_rejects_bad_header", test_format_rtf_import_rejects_bad_header },
    { "format_rtf_import_rejects_unsupported_charset", test_format_rtf_import_rejects_unsupported_charset },
    { "format_rtf_import_simple_paragraph", test_format_rtf_import_simple_paragraph },
    { "format_rtf_import_no_trailing_empty_paragraph", test_format_rtf_import_no_trailing_empty_paragraph },
    { "format_rtf_import_blank_line_is_empty_paragraph", test_format_rtf_import_blank_line_is_empty_paragraph },
    { "format_rtf_import_bold_italic_underline", test_format_rtf_import_bold_italic_underline },
    { "format_rtf_import_font_size", test_format_rtf_import_font_size },
    { "format_rtf_import_alignment_and_indents", test_format_rtf_import_alignment_and_indents },
    { "format_rtf_import_color_table", test_format_rtf_import_color_table },
    { "format_rtf_import_hex_escape_decodes_cp1252", test_format_rtf_import_hex_escape_decodes_cp1252 },
    { "format_rtf_import_special_chars", test_format_rtf_import_special_chars },
    { "format_rtf_import_skips_fonttbl_and_ignorable_destination", test_format_rtf_import_skips_fonttbl_and_ignorable_destination },
    { "format_rtf_import_unrecognized_control_word_is_ignored", test_format_rtf_import_unrecognized_control_word_is_ignored },
    { "format_rtf_import_truncated_hex_escape_is_badfmt", test_format_rtf_import_truncated_hex_escape_is_badfmt },
    { "format_rtf_import_exceeds_max_nesting_depth_is_range", test_format_rtf_import_exceeds_max_nesting_depth_is_range },
    { "format_rtf_import_truncated_input_recovers_leniently", test_format_rtf_import_truncated_input_recovers_leniently },
    { "format_rtf_import_no_leaks", test_format_rtf_import_no_leaks },

    { "format_rtf_export_reports_size_on_overflow", test_format_rtf_export_reports_size_on_overflow },
    { "format_rtf_roundtrip_plain_text", test_format_rtf_roundtrip_plain_text },
    { "format_rtf_roundtrip_full_attribute_set", test_format_rtf_roundtrip_full_attribute_set },

    { "spell_dict_open_missing_file_fails", test_spell_dict_open_missing_file_fails },
    { "spell_check_exact_known_word", test_spell_check_exact_known_word },
    { "spell_check_unknown_word", test_spell_check_unknown_word },
    { "spell_check_case_insensitive", test_spell_check_case_insensitive },
    { "spell_check_affix_rules", test_spell_check_affix_rules },
    { "spell_check_non_ascii_fails_open", test_spell_check_non_ascii_fails_open },
    { "spell_check_no_leaks", test_spell_check_no_leaks },
    { "spell_suggest_finds_transposition", test_spell_suggest_finds_transposition },
    { "spell_suggest_respects_max", test_spell_suggest_respects_max },
    { "spell_suggest_non_ascii_returns_nothing", test_spell_suggest_non_ascii_returns_nothing },
    { "spell_realistic_dictionary_sample", test_spell_realistic_dictionary_sample },

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
    { "offload_grammar_check_known_fixture", test_offload_grammar_check_known_fixture },
    { "offload_grammar_check_clean_paragraph_returns_no_issues", test_offload_grammar_check_clean_paragraph_returns_no_issues },
    { "offload_chat_happy_path", test_offload_chat_happy_path },
    { "offload_chat_unrecognized_prompt_gets_generic_reply", test_offload_chat_unrecognized_prompt_gets_generic_reply },
    { "offload_capabilities_advertise_chat_but_not_image", test_offload_capabilities_advertise_chat_but_not_image },
    { "offload_unsupported_op_reports_unsupported", test_offload_unsupported_op_reports_unsupported },
    { "offload_corrupt_response_is_rejected", test_offload_corrupt_response_is_rejected },
    { "offload_timeout_on_delayed_gateway", test_offload_timeout_on_delayed_gateway },
    { "offload_gateway_dies_before_responding", test_offload_gateway_dies_before_responding },
    { "offload_short_frame_times_out", test_offload_short_frame_times_out },
    { "offload_submit_fails_after_gateway_killed", test_offload_submit_fails_after_gateway_killed },
    { "convert_odt_import_via_gateway", test_convert_odt_import_via_gateway },
    { "convert_odt_export_roundtrip_via_gateway", test_convert_odt_export_roundtrip_via_gateway },
    { "convert_pdf_export_via_gateway", test_convert_pdf_export_via_gateway },
    { "convert_unsupported_format_id_reports_unsupported", test_convert_unsupported_format_id_reports_unsupported },
    { "convert_malformed_payload_is_error", test_convert_malformed_payload_is_error },

    { "grammar_doubled_word_flags_whole_phrase_and_suggests_one_copy", test_grammar_doubled_word_flags_whole_phrase_and_suggests_one_copy },
    { "grammar_doubled_word_is_case_insensitive", test_grammar_doubled_word_is_case_insensitive },
    { "grammar_no_doubled_word_for_different_words", test_grammar_no_doubled_word_for_different_words },
    { "grammar_triple_word_reports_one_issue_and_moves_past_it", test_grammar_triple_word_reports_one_issue_and_moves_past_it },
    { "grammar_multiple_spaces_suggests_single_space", test_grammar_multiple_spaces_suggests_single_space },
    { "grammar_single_space_is_not_flagged", test_grammar_single_space_is_not_flagged },
    { "grammar_space_before_punctuation_includes_the_mark", test_grammar_space_before_punctuation_includes_the_mark },
    { "grammar_no_space_before_punctuation_is_not_flagged", test_grammar_no_space_before_punctuation_is_not_flagged },
    { "grammar_repeated_exclamation_flagged", test_grammar_repeated_exclamation_flagged },
    { "grammar_ellipsis_is_not_flagged_as_repeated_punctuation", test_grammar_ellipsis_is_not_flagged_as_repeated_punctuation },
    { "grammar_missing_initial_capital_flagged", test_grammar_missing_initial_capital_flagged },
    { "grammar_capitalized_start_is_not_flagged", test_grammar_capitalized_start_is_not_flagged },
    { "grammar_clean_text_reports_no_issues", test_grammar_clean_text_reports_no_issues },
    { "grammar_empty_text_reports_no_issues", test_grammar_empty_text_reports_no_issues },
    { "grammar_max_issues_cap_is_respected", test_grammar_max_issues_cap_is_respected },
    { "grammar_realistic_paragraph_finds_every_planted_issue", test_grammar_realistic_paragraph_finds_every_planted_issue }
};

int main(void)
{
    int count = (int)(sizeof(g_cases) / sizeof(g_cases[0]));
    return test_run_all(g_cases, count);
}
