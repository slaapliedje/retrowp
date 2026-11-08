/* SPDX-License-Identifier: GPL-2.0-or-later */
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

/* platform_style.c */
void test_platform_style_bits_match_document_model(void);
void test_platform_style_bits_do_not_collide_with_document_only_bits(void);

/* menucheck.c */
void test_menucheck_accel_letter_maps_only_ctrl_letters(void);
void test_menucheck_accel_letter_0x0c_is_ctrl_l(void);
void test_menucheck_label_rejects_menu_metacharacters(void);
void test_menucheck_live_action_table_is_menu_safe(void);
void test_menucheck_every_live_label_is_literal_safe(void);
void test_menucheck_shortcut_bytes_are_unique(void);
void test_menucheck_no_menu_group_is_empty(void);
void test_menucheck_help_actions_name_real_help_nodes(void);
void test_menucheck_help_node_tolerates_null_len(void);

/* edit_render.c */
void test_edit_render_typed_text_is_visible_after_relayout(void);
void test_edit_render_delete_marks_layout_dirty_and_stays_in_bounds(void);
void test_edit_render_insert_marks_layout_dirty_and_stays_in_bounds(void);
void test_edit_render_deleted_text_disappears_after_relayout(void);
void test_edit_render_clean_paragraphs_are_not_relayouted(void);

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
void test_edit_split_paragraph_mid_run(void);
void test_edit_split_paragraph_at_start(void);
void test_edit_split_paragraph_at_end(void);
void test_edit_split_paragraph_of_empty_paragraph(void);
void test_edit_merge_paragraph_basic(void);
void test_edit_merge_paragraph_rejects_last_paragraph(void);
void test_edit_split_then_merge_roundtrips(void);

/* test_selection.c */
void test_selection_pos_compare_orders_correctly(void);
void test_selection_extract_text_single_paragraph(void);
void test_selection_extract_text_multi_paragraph(void);
void test_selection_extract_text_reports_size_on_overflow(void);
void test_selection_delete_single_paragraph(void);
void test_selection_delete_empty_selection_is_noop(void);
void test_selection_delete_multi_paragraph_then_undo_then_redo(void);
void test_selection_delete_out_pos_lands_at_delete_start(void);
void test_selection_delete_swallows_whole_middle_paragraph(void);
void test_selection_delete_no_leaks(void);

/* motion.c */
void test_motion_word_forward_skips_word_then_blank(void);
void test_motion_word_forward_stops_at_punctuation_boundary(void);
void test_motion_word_forward_big_word_merges_punct(void);
void test_motion_word_backward_returns_to_word_starts(void);
void test_motion_word_end_lands_after_last_char_of_word(void);
void test_motion_word_forward_crosses_paragraph_boundary(void);
void test_motion_line_start_end_first_nonblank(void);
void test_motion_line_first_nonblank_on_all_blank_line_lands_at_end(void);
void test_motion_paragraph_down_up(void);
void test_motion_para_block_forward_backward_finds_empty_paragraphs(void);

/* vimode.c */
void test_vimode_insert_then_escape(void);
void test_vimode_hjkl_and_word_motion(void);
void test_vimode_count_prefix_motion(void);
void test_vimode_dw_deletes_word(void);
void test_vimode_dw_does_not_eat_paragraph_break(void);
void test_vimode_dd_deletes_whole_line(void);
void test_vimode_count_dd_deletes_multiple_lines(void);
void test_vimode_d_count_motion_deletes_multiple_lines(void);
void test_vimode_count_dd_undoes_as_one_step(void);
void test_vimode_dollar_deletes_to_end_of_line(void);
void test_vimode_dd_on_last_paragraph_merges_upward(void);
void test_vimode_yy_p_duplicates_line_below(void);
void test_vimode_cw_change_word(void);
void test_vimode_insert_session_undoes_as_one_step(void);
void test_vimode_cw_undoes_as_one_step(void);
void test_vimode_visual_mode_delete(void);
void test_vimode_visual_line_mode_delete_spans_paragraphs(void);
void test_vimode_ex_command_save_quit(void);
void test_vimode_ex_save_with_filename(void);
void test_vimode_ex_open_command(void);
void test_vimode_ex_filename_edge_cases(void);
void test_vimode_search_forward(void);
void test_vimode_search_backward(void);
void test_vimode_n_and_capital_n_respect_search_direction(void);
void test_vimode_substitute_current_paragraph_replaces_first_match_only(void);
void test_vimode_substitute_global_flag_replaces_every_match_in_paragraph(void);
void test_vimode_substitute_percent_scopes_to_whole_section(void);
void test_vimode_substitute_percent_and_global_replaces_everywhere(void);
void test_vimode_substitute_ignore_case_flag(void);
void test_vimode_substitute_confirm_y_replaces_n_skips(void);
void test_vimode_substitute_confirm_a_replaces_all_remaining(void);
void test_vimode_substitute_confirm_q_stops_without_replacing(void);
void test_vimode_substitute_malformed_command_falls_through_to_beep(void);
void test_vimode_substitute_global_undoes_as_one_step(void);
void test_vimode_o_opens_line_below(void);
void test_vimode_x_deletes_char_forward(void);
void test_vimode_ctrl_chord_is_not_bare_letter(void);
void test_vimode_alt_chord_is_not_bare_letter(void);
void test_vimode_no_leaks(void);

/* test_wordstar.c */
void test_wordstar_diamond_char_motions(void);
void test_wordstar_diamond_word_motions(void);
void test_wordstar_diamond_paragraph_motions(void);
void test_wordstar_arrow_keys_also_move(void);
void test_wordstar_plain_typing_is_non_modal(void);
void test_wordstar_ctrl_g_deletes_char_forward(void);
void test_wordstar_ctrl_t_deletes_word_forward(void);
void test_wordstar_ctrl_y_deletes_current_paragraph(void);
void test_wordstar_block_copy_appends_at_cursor(void);
void test_wordstar_block_move_cursor_before_block(void);
void test_wordstar_block_move_cursor_after_block(void);
void test_wordstar_block_move_refuses_when_cursor_inside_block(void);
void test_wordstar_block_delete(void);
void test_wordstar_kx_returns_save_quit_and_kq_returns_quit(void);
void test_wordstar_unbound_prefix_subkey_beeps(void);
void test_wordstar_format_prefix_returns_correct_kinds(void);
void test_wordstar_find_jumps_to_match(void);
void test_wordstar_find_not_found_beeps(void);
void test_wordstar_find_empty_text_beeps(void);
void test_wordstar_find_escape_cancels_prompt(void);
void test_wordstar_find_backward_option(void);
void test_wordstar_find_ignore_case_and_whole_word_options(void);
void test_wordstar_replace_confirm_y_and_n(void);
void test_wordstar_replace_no_confirm_option_replaces_silently(void);
void test_wordstar_replace_global_option_restarts_from_document_start(void);
void test_wordstar_ctrl_l_repeats_last_find(void);
void test_wordstar_ctrl_l_without_prior_search_beeps(void);
void test_wordstar_no_leaks(void);

/* test_emacs.c */
void test_emacs_basic_char_and_line_motion(void);
void test_emacs_line_start_and_end(void);
void test_emacs_meta_word_motion_via_alt_bit(void);
void test_emacs_meta_via_escape_prefix_fallback(void);
void test_emacs_meta_doc_start_and_end(void);
void test_emacs_plain_typing_is_non_modal(void);
void test_emacs_ctrl_d_and_backspace_delete(void);
void test_emacs_meta_d_kills_word_forward_and_ctrl_y_yanks(void);
void test_emacs_meta_backspace_kills_word_backward(void);
void test_emacs_ctrl_k_kills_to_line_end_then_merges_paragraph(void);
void test_emacs_mark_and_kill_region(void);
void test_emacs_kill_region_without_mark_beeps(void);
void test_emacs_meta_w_copies_region_without_moving_cursor(void);
void test_emacs_exchange_point_and_mark(void);
void test_emacs_ctrl_x_prefix_save_open_quit(void);
void test_emacs_ctrl_x_u_undoes(void);
void test_emacs_ctrl_c_prefix_format_bindings(void);
void test_emacs_ctrl_g_cancels_pending_prefix(void);
void test_emacs_unbound_chord_beeps(void);
void test_emacs_isearch_forward_lands_at_match_end(void);
void test_emacs_isearch_backward_lands_at_match_start(void);
void test_emacs_isearch_ctrl_s_repeats_forward(void);
void test_emacs_isearch_backspace_shrinks_pattern_and_researches(void);
void test_emacs_isearch_failing_search_does_not_move_cursor(void);
void test_emacs_isearch_ctrl_g_aborts_to_anchor(void);
void test_emacs_query_replace_confirm_vocabulary(void);
void test_emacs_query_replace_bang_replaces_all_remaining(void);
void test_emacs_query_replace_not_found_beeps(void);
void test_emacs_query_replace_escape_cancels_prompt(void);
void test_emacs_no_leaks(void);

/* test_wordperfect.c */
void test_wordperfect_arrow_char_motion(void);
void test_wordperfect_ctrl_arrow_word_motion(void);
void test_wordperfect_up_down_paragraph_motion(void);
void test_wordperfect_home_end(void);
void test_wordperfect_ctrl_home_goes_to_doc_start(void);
void test_wordperfect_plain_typing_is_non_modal(void);
void test_wordperfect_delete_forward_and_backward(void);
void test_wordperfect_ctrl_backspace_deletes_word_backward(void);
void test_wordperfect_ctrl_end_deletes_to_line_end(void);
void test_wordperfect_f1_undelete_is_single_level_undo(void);
void test_wordperfect_f6_f8_f7_f10_return_correct_kinds(void);
void test_wordperfect_block_toggle_extends_by_motion(void);
void test_wordperfect_move_block_then_retrieve(void);
void test_wordperfect_ctrl_f4_without_block_beeps(void);
void test_wordperfect_shift_f10_with_empty_register_beeps(void);
void test_wordperfect_search_forward_jumps_to_match(void);
void test_wordperfect_search_backward(void);
void test_wordperfect_search_empty_text_beeps(void);
void test_wordperfect_search_not_found_beeps(void);
void test_wordperfect_search_escape_cancels_prompt(void);
void test_wordperfect_search_f2_twice_repeats_last_search(void);
void test_wordperfect_replace_confirm_y_and_n(void);
void test_wordperfect_replace_no_confirm_replaces_silently(void);
void test_wordperfect_no_leaks(void);

/* test_msword.c */
void test_msword_arrow_char_motion(void);
void test_msword_shift_arrow_extends_selection(void);
void test_msword_ctrl_arrow_word_motion_and_shift_ctrl_extends_by_word(void);
void test_msword_home_end_and_ctrl_home_end(void);
void test_msword_plain_typing_is_non_modal(void);
void test_msword_typing_over_selection_replaces_it(void);
void test_msword_delete_and_backspace_without_selection(void);
void test_msword_delete_with_active_selection_deletes_the_selection(void);
void test_msword_ctrl_delete_and_backspace_delete_whole_words(void);
void test_msword_enter_with_selection_deletes_it_first(void);
void test_msword_ctrl_a_selects_all(void);
void test_msword_ctrl_z_undo_and_ctrl_y_redo(void);
void test_msword_ctrl_letter_action_bindings(void);
void test_msword_unbound_ctrl_letter_beeps(void);
void test_msword_ctrl_q_quits(void);
void test_msword_find_jumps_to_match(void);
void test_msword_find_wraps_around_document(void);
void test_msword_find_not_found_beeps(void);
void test_msword_find_escape_cancels_prompt(void);
void test_msword_shift_f4_repeats_last_find(void);
void test_msword_shift_f4_without_prior_find_beeps(void);
void test_msword_replace_r_and_n(void);
void test_msword_replace_all_replaces_silently(void);
void test_msword_replace_confirm_escape_closes(void);
void test_msword_no_leaks(void);

/* test_actions.c */
void test_actions_find_by_id(void);

/* format_rtf_image.c */
void test_rtf_image_exports_a_real_pict(void);
void test_rtf_image_round_trips_through_export_and_import(void);
void test_rtf_image_import_without_a_sink_skips_the_picture(void);
void test_rtf_image_import_keeps_the_text_around_a_bad_picture(void);
void test_rtf_image_dib_header_is_well_formed(void);
void test_rtf_image_rows_are_bottom_up_and_padded(void);
void test_rtf_image_without_a_source_is_an_honest_placeholder(void);
void test_rtf_image_export_is_not_an_empty_paragraph(void);
void test_rtf_image_missing_cache_file_still_writes_the_promised_size(void);
void test_rtf_image_text_paragraphs_are_unaffected(void);
void test_actions_bold_styles_the_selection(void);
void test_actions_bold_toggles_input_style_with_no_selection(void);
void test_actions_bold_on_an_all_bold_selection_clears_it(void);
void test_actions_find_by_shortcut(void);
void test_actions_every_shortcut_is_unique(void);
void test_actions_table_row_order_matches_enum(void);
void test_actions_every_action_is_reachable(void);
void test_actions_every_action_has_a_label(void);
void test_actions_format_bold_toggles_style_flags(void);
void test_actions_file_quit_sets_flag(void);
void test_actions_edit_copy_sets_clipboard(void);
void test_actions_format_size_uses_dialog_choice(void);
void test_actions_cut_then_paste_gives_the_document_back(void);
void test_actions_cut_takes_every_paragraph_of_an_opened_file(void);
void test_actions_cut_copes_with_a_run_of_no_text(void);
void test_actions_cut_is_one_undo_step(void);
void test_actions_cut_keeps_a_document_the_clipboard_cannot_hold(void);
void test_viewport_counts_display_lines_across_paragraphs(void);
void test_viewport_follows_the_cursor_with_the_least_scroll(void);
void test_viewport_follow_keeps_the_top_inside_a_shorter_document(void);
void test_viewport_page_moves_view_and_cursor_together(void);
void test_viewport_page_lands_mid_paragraph_on_a_wrapped_line(void);
void test_viewport_scroll_by_hand_holds_until_a_keystroke(void);
void test_viewport_page_keys_work_under_every_scheme(void);
void test_viewport_replace_document_goes_back_to_the_top(void);
void test_viewport_finds_the_line_under_y_and_the_position_under_a_click(void);
void test_viewport_click_moves_or_extends_under_every_scheme(void);
void test_tools_grammar_check_reports_locally_without_a_gateway(void);
void test_tools_grammar_and_chat_want_a_paragraph_with_text(void);
void test_tools_gateway_tools_say_when_there_is_no_device(void);
void test_tools_spell_check_names_the_first_unknown_word(void);
void test_format_md_headings_become_big_bold_paragraphs(void);
void test_format_md_emphasis_makes_styled_runs(void);
void test_format_md_joins_wrapped_lines_and_splits_blocks(void);
void test_format_md_leaves_text_that_only_looks_like_markup(void);
void test_format_md_nested_and_adjacent_emphasis(void);
void test_format_md_export_round_trips_what_the_model_holds(void);
void test_format_md_export_of_a_plain_document_is_its_paragraphs(void);
void test_actions_copy_takes_only_the_selection(void);
void test_actions_cut_takes_only_the_selection(void);
void test_actions_paste_replaces_the_selection(void);
void test_actions_copy_and_cut_with_nothing_selected_do_nothing(void);
void test_actions_cut_in_visual_line_takes_whole_paragraphs(void);
void test_actions_save_file_picks_the_format_by_extension_and_remembers(void);
void test_actions_save_goes_to_the_remembered_name_without_asking(void);
void test_actions_load_file_reads_text_and_rtf_and_keeps_the_name(void);
void test_actions_odt_and_pdf_go_through_the_gateway(void);
void test_actions_file_print_routes(void);
void test_actions_file_print_options(void);
void test_actions_file_print_choose_printer(void);
void test_config_printer_keys(void);
void test_pageprint_pages(void);
void test_pageprint_printable_area(void);
void test_config_save_printer(void);
void test_actions_file_open_asks_picks_and_a_new_document_forgets_the_name(void);
void test_actions_paragraph_buttons_change_the_caret_paragraph(void);
void test_actions_toggle_tab_adds_snaps_and_removes(void);
void test_rulerbar_hit_test_finds_buttons_gaps_and_the_ruler(void);
void test_rulerbar_signature_follows_what_the_bar_shows(void);
void test_actions_page_break_splits_and_undo_merges(void);
void test_actions_header_prompt_types_edits_and_sets(void);
void test_actions_document_name_is_the_last_path_component(void);
void test_selspan_marks_the_lines_a_selection_crosses(void);
void test_selspan_within_one_line(void);
void test_selspan_starting_at_a_line_end_shows_nothing_there(void);
void test_actions_paste_inserts_at_the_cursor(void);
void test_actions_request_quit_asks_when_modified(void);
void test_actions_save_quit_stays_when_the_save_did_not_land(void);
void test_actions_replace_document_resets_the_journal(void);
void test_actions_write_file_marks_saved_only_when_it_lands(void);
void test_actions_align_marks_the_document_modified(void);
void test_actions_cut_with_an_image_paragraph(void);

/* test_statusline.c */
void test_statusline_paint_does_not_crash_with_various_views(void);

/* test_config.c */
void test_config_defaults_to_vi_on_empty_input(void);
void test_config_parses_keymap_vim(void);
void test_config_parses_keymap_wordstar(void);
void test_config_parses_keymap_emacs(void);
void test_config_parses_keymap_wordperfect(void);
void test_config_parses_keymap_msword(void);
void test_config_key_and_value_are_case_insensitive(void);
void test_config_tolerates_surrounding_whitespace(void);
void test_config_skips_comment_and_blank_lines(void);
void test_config_unrecognized_value_falls_back_to_default(void);
void test_config_unrecognized_key_is_ignored(void);
void test_config_malformed_line_without_equals_is_skipped(void);
void test_config_tolerates_crlf_line_endings(void);
void test_config_last_line_without_trailing_newline_is_parsed(void);
void test_config_load_missing_file_yields_default(void);
void test_config_shipped_sample_parses(void);
void test_config_sample_inert_keys_do_not_alter_defaults(void);

/* test_keymap.c */
void test_keymap_app_ctx_size_stays_under_ceiling(void);
void test_keymap_init_selects_vi_and_seeds_cursor_at_doc_start(void);
void test_keymap_feed_key_moves_cursor_through_vi_arm(void);
void test_keymap_feed_key_insert_reports_inserted_char(void);
void test_keymap_feed_key_quit_reports_quit_kind(void);
void test_keymap_ex_filename_reaches_outcome(void);
void test_keymap_get_status_view_reports_vi_mode_label(void);

/* test_search.c (M11) */
void test_search_find_forward_basic(void);
void test_search_find_forward_not_found(void);
void test_search_find_forward_wraps_around_document(void);
void test_search_find_backward_basic(void);
void test_search_find_backward_wraps_around_document(void);
void test_search_find_backward_picks_nearest_preceding_match(void);
void test_search_ignore_case_matches_different_case(void);
void test_search_whole_word_rejects_substring_match(void);
void test_search_whole_word_false_matches_substring(void);
void test_search_match_crosses_run_boundary(void);
void test_search_match_selection_within_single_run(void);
void test_search_empty_needle_is_error(void);

/* test_help_doc.c */
void test_help_doc_parses_single_node(void);
void test_help_doc_node_without_title_falls_back_to_name(void);
void test_help_doc_parses_link_line(void);
void test_help_doc_braces_without_bar_are_plain_text(void);
void test_help_doc_find_node_by_name(void);
void test_help_doc_missing_endnode_at_eof_implicitly_closes(void);
void test_help_doc_skips_content_before_first_node(void);
void test_help_doc_blank_line_preserved_as_empty_text_line(void);
void test_help_doc_empty_source_yields_zero_nodes(void);
void test_help_doc_no_leaks(void);

/* test_help_view.c */
void test_help_wrap_line_greedy_word_wrap_hand_computed(void);
void test_help_wrap_line_overlong_word_gets_its_own_row(void);
void test_help_wrap_line_empty_text_yields_zero_rows(void);
void test_help_wrap_line_respects_max_rows_cap(void);
void test_help_view_init_clamps_start_node(void);
void test_help_view_escape_and_q_request_close(void);
void test_help_view_link_selection_clamped_at_bounds(void);
void test_help_view_enter_follows_link_and_backspace_goes_back(void);
void test_help_view_enter_on_broken_link_target_is_a_noop(void);
void test_help_view_page_scroll_clamped_at_zero(void);
void test_help_view_paint_does_not_crash_with_various_states(void);
void test_help_view_no_leaks(void);

/* test_help_content.c */
void test_help_content_retrowp_help_parses_and_all_links_resolve(void);

/* linebreak.c */
void test_layout_greedy_wrap_hand_computed_fixture(void);
void test_layout_geometry_indent_and_alignment_place_the_line(void);
void test_layout_geometry_later_lines_use_the_left_indent_only(void);
void test_layout_geometry_spacing_shapes_the_line_boxes(void);
void test_layout_geometry_default_tabs_advance_to_half_inches(void);
void test_layout_geometry_a_tab_can_start_a_line(void);
void test_layout_geometry_right_and_centre_stops(void);
void test_layout_geometry_fragments_caret_and_clicks_agree_on_tabs(void);
void test_layout_geometry_styles_with_different_tabs_stay_apart(void);
void test_layout_geometry_page_break_before_starts_a_page(void);
void test_layout_single_word_wider_than_column_overflows(void);
void test_layout_empty_paragraph_gets_one_zero_width_line(void);
void test_layout_word_spanning_run_boundary_is_one_token(void);
void test_layout_editing_one_paragraph_does_not_remeasure_whole_document(void);

/* justify.c */
void test_justify_non_justify_align_returns_zero(void);
void test_justify_pos_at_x_finds_the_nearest_boundary(void);
void test_justify_last_line_of_paragraph_returns_zero(void);
void test_justify_computes_extra_per_gap(void);
void test_justify_single_word_line_has_no_gap(void);
void test_justify_fragment_iter_splits_at_run_boundary_not_at_word(void);
void test_justify_cursor_x_measures_from_line_start_not_paragraph_start(void);
void test_justify_cursor_x_past_line_end_clamps_to_line_width(void);
void test_justify_cursor_x_accumulates_justify_slack(void);
void test_justify_cursor_x_crosses_run_boundaries(void);
void test_justify_cursor_x_in_skipped_zero_length_run(void);

/* paginate.c */
void test_paginate_known_page_size_yields_expected_page_count(void);
void test_paginate_new_section_always_starts_new_page(void);
void test_paginate_no_leaks(void);

/* undo.c */
void test_undo_insert_then_undo_then_redo(void);

/* style_range.c */
void test_style_range_bolds_a_word_in_the_middle(void);
void test_style_range_splits_the_run_it_partially_covers(void);
void test_style_range_marks_the_paragraph_for_relayout(void);
void test_style_range_marks_the_document_modified(void);
void test_style_range_undo_restores_every_run_then_redo_reapplies(void);
void test_style_range_undo_of_a_partial_range_restores_both_fragments(void);
void test_style_range_keeps_each_runs_own_size(void);
void test_style_range_clear_turns_a_flag_back_off(void);
void test_style_range_accepts_its_ends_in_either_order(void);
void test_style_range_empty_range_changes_nothing(void);
void test_style_range_spans_paragraphs(void);
void test_range_has_flags_answers_the_toggle_question(void);
void test_undo_delete_all_cases_then_undo_redo(void);
void test_undo_new_edit_clears_redo(void);
void test_undo_empty_journal_returns_error(void);
void test_ws_soft_return_joins_hard_return_splits(void);
void test_ws_soft_return_does_not_double_space(void);
void test_ws_high_bit_is_stripped(void);
void test_ws_style_toggles_produce_styled_runs(void);
void test_ws_dot_commands_dropped_but_prose_kept(void);
void test_ws_blank_lines_survive(void);
void test_ws_sniff_rejects_plain_text(void);
void test_ws_garbage_still_imports(void);
void test_ws_import_is_not_modified(void);
void test_undo_no_leaks(void);
void test_document_import_is_not_modified(void);
void test_journaled_edit_marks_document_modified(void);
void test_failed_undo_does_not_mark_modified(void);
void test_real_undo_marks_modified(void);
void test_undo_fuzz_n_ops_then_n_undos_restores_original(void);
void test_undo_split_paragraph_then_undo_then_redo(void);
void test_undo_merge_paragraph_then_undo_then_redo(void);
void test_undo_split_merge_no_leaks(void);

/* strpool.c */
void test_strpool_reserves_offset_zero(void);
void test_strpool_multiple_appends_survive_growth(void);
void test_strpool_empty_append(void);

/* docmodel.c */
void test_docmodel_init_free_has_no_leaks(void);
void test_docmodel_init_sets_usable_defaults(void);
void test_docmodel_cstyle_add_survives_growth(void);
void test_docmodel_cstyle_find_or_add_reuses_identical_style(void);
void test_docmodel_pstyle_find_or_add_reuses_identical_style(void);
void test_docmodel_build_known_tree(void);
void test_docmodel_pstyle_and_cstyle_set(void);

/* test_image.c (M15 Phase 1) */
void test_image_add_get_roundtrip(void);
void test_image_merge_absorbing_a_picture_undoes_to_the_picture(void);
void test_image_merge_text_onto_a_picture_keeps_the_text(void);
void test_image_undoing_enter_on_a_picture_keeps_the_picture(void);
void test_image_add_survives_growth(void);
void test_edit_insert_image_splits_paragraph(void);
void test_edit_insert_image_undo_redo(void);

/* test_image_layout.c (M15 Phase 2) */
void test_image_layout_relayout_produces_one_line_from_descriptor(void);
void test_image_layout_missing_handle_degrades_to_zero_size(void);
void test_image_layout_fragment_iter_returns_false_immediately(void);
void test_image_layout_justify_extra_is_zero(void);
void test_image_layout_paginate_mixed_text_and_image(void);

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
void test_format_prn_letter_page(void);
void test_format_prn_crlf(void);
void test_format_prn_wraps_at_words(void);
void test_format_prn_cuts_a_long_word(void);
void test_format_prn_alignment(void);
void test_format_prn_indents_and_tabs(void);
void test_format_prn_spacing(void);
void test_format_prn_pages_header_footer(void);
void test_format_prn_page_break_and_top_space(void);
void test_format_prn_encodings(void);
void test_format_prn_empty_document(void);
void test_format_prn_epson(void);
void test_format_prn_pcl(void);
void test_format_prn_pcl_paper(void);
void test_format_prn_amiga(void);
void test_format_prn_postscript(void);
void test_format_prn_driver_empty(void);
void test_format_prn_postscript_file(void);

/* format_rtf_import.c */
void test_format_rtf_import_rejects_bad_header(void);
void test_format_rtf_import_decodes_other_code_pages(void);
void test_fonts_table_finds_by_name_ignoring_case(void);
void test_fonts_family_guess_from_a_bare_name(void);
void test_fonts_rtf_word_font_table_and_charsets(void);
void test_fonts_rtf_default_font_and_plain(void);
void test_fonts_rtf_round_trip_keeps_faces_and_the_default(void);
void test_fonts_face_pick_by_name_then_family(void);
void test_fonts_family_bits_match_the_seam(void);
void test_actions_font_restyles_the_selection_as_one_step(void);
void test_actions_typed_text_keeps_its_neighbours_style_unless_armed(void);
void test_rulerbar_has_font_and_size(void);
void test_convert_pdf_export_picks_faces_by_family(void);
void test_rulerbar_panel_look_follows_the_colour_count(void);
void test_rulerbar_dragging_indents(void);
void test_rulerbar_dragging_tabs(void);
void test_convert_odt_carries_headers_footers_and_the_page(void);
void test_convert_a_reply_in_parts_arrives_whole(void);
void test_rtf_modern_sections_in(void);
void test_rtf_modern_sections_round_trip(void);
void test_convert_sections_reach_the_pdf_and_odt(void);
void test_actions_header_follows_the_caret_section(void);
void test_actions_page_setup_paper_and_margins(void);
void test_actions_section_break_and_backspace(void);
void test_actions_delete_across_a_section_break(void);
void test_convert_a_large_document_goes_in_parts(void);
void test_rtf_modern_unicode_and_its_fallback(void);
void test_rtf_modern_plain_resets_and_hidden_text_stays_out(void);
void test_rtf_modern_word_noise_stays_out_of_the_body(void);
void test_rtf_modern_table_rows_become_tabbed_paragraphs(void);
void test_rtf_modern_line_spacing_tabs_and_page_setup(void);
void test_rtf_modern_alike_paragraphs_share_one_style(void);
void test_rtf_modern_export_round_trips_the_new_properties(void);
void test_rtf_modern_page_breaks_both_ways(void);
void test_rtf_modern_headers_and_footers_both_ways(void);
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
void test_offload_capabilities_advertise_chat_but_not_convert_import(void);
void test_offload_unsupported_op_reports_unsupported(void);
void test_offload_capabilities_advertise_image(void);
void test_offload_image_returns_canned_fixture_bitmap(void);
void test_offload_capabilities_advertise_dictate(void);
void test_offload_dictate_returns_canned_transcript(void);
void test_offload_dictate_rejects_malformed_request(void);
void test_offload_corrupt_response_is_rejected(void);
void test_offload_timeout_on_delayed_gateway(void);
void test_offload_gateway_dies_before_responding(void);
void test_offload_short_frame_times_out(void);
void test_offload_submit_fails_after_gateway_killed(void);

/* offload/gateway/odt_gateway.py (M7 T7.2) */
void test_convert_odt_import_via_gateway(void);
void test_convert_print_needs_a_printer(void);
void test_convert_print_to_a_folder(void);
void test_convert_print_through_lp(void);
void test_convert_hello_mid_session_is_a_new_handshake(void);
void test_convert_gateway_resyncs_past_noise(void);
void test_convert_request_without_hello(void);
void test_convert_print_options_listed(void);
void test_convert_print_option_refused(void);
void test_convert_odt_export_roundtrip_via_gateway(void);
void test_convert_pdf_export_via_gateway(void);
void test_convert_pdf_export_uses_the_document_page(void);
void test_convert_pdf_export_honours_page_breaks(void);
void test_convert_unsupported_format_id_reports_unsupported(void);
void test_convert_malformed_payload_is_error(void);

/* test_image_convert.c (M15 Phase 3) */
void test_image_convert_real_pillow_color_round_trip(void);
void test_image_convert_real_pillow_grayscale_round_trip(void);
void test_image_convert_pillow_unavailable_reports_unsupported(void);
void test_image_convert_malformed_request_is_error(void);

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
    { "platform_style_bits_match_document_model", test_platform_style_bits_match_document_model },
    { "platform_style_bits_do_not_collide_with_document_only_bits", test_platform_style_bits_do_not_collide_with_document_only_bits },

    { "menucheck_accel_letter_maps_only_ctrl_letters", test_menucheck_accel_letter_maps_only_ctrl_letters },
    { "menucheck_accel_letter_0x0c_is_ctrl_l", test_menucheck_accel_letter_0x0c_is_ctrl_l },
    { "menucheck_label_rejects_menu_metacharacters", test_menucheck_label_rejects_menu_metacharacters },
    { "menucheck_live_action_table_is_menu_safe", test_menucheck_live_action_table_is_menu_safe },
    { "menucheck_every_live_label_is_literal_safe", test_menucheck_every_live_label_is_literal_safe },
    { "menucheck_shortcut_bytes_are_unique", test_menucheck_shortcut_bytes_are_unique },
    { "menucheck_no_menu_group_is_empty", test_menucheck_no_menu_group_is_empty },
    { "menucheck_help_actions_name_real_help_nodes", test_menucheck_help_actions_name_real_help_nodes },
    { "menucheck_help_node_tolerates_null_len", test_menucheck_help_node_tolerates_null_len },

    { "edit_render_typed_text_is_visible_after_relayout", test_edit_render_typed_text_is_visible_after_relayout },
    { "edit_render_delete_marks_layout_dirty_and_stays_in_bounds", test_edit_render_delete_marks_layout_dirty_and_stays_in_bounds },
    { "edit_render_insert_marks_layout_dirty_and_stays_in_bounds", test_edit_render_insert_marks_layout_dirty_and_stays_in_bounds },
    { "edit_render_deleted_text_disappears_after_relayout", test_edit_render_deleted_text_disappears_after_relayout },
    { "edit_render_clean_paragraphs_are_not_relayouted", test_edit_render_clean_paragraphs_are_not_relayouted },

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
    { "edit_split_paragraph_mid_run", test_edit_split_paragraph_mid_run },
    { "edit_split_paragraph_at_start", test_edit_split_paragraph_at_start },
    { "edit_split_paragraph_at_end", test_edit_split_paragraph_at_end },
    { "edit_split_paragraph_of_empty_paragraph", test_edit_split_paragraph_of_empty_paragraph },
    { "edit_merge_paragraph_basic", test_edit_merge_paragraph_basic },
    { "edit_merge_paragraph_rejects_last_paragraph", test_edit_merge_paragraph_rejects_last_paragraph },
    { "edit_split_then_merge_roundtrips", test_edit_split_then_merge_roundtrips },

    { "selection_pos_compare_orders_correctly", test_selection_pos_compare_orders_correctly },
    { "selection_extract_text_single_paragraph", test_selection_extract_text_single_paragraph },
    { "selection_extract_text_multi_paragraph", test_selection_extract_text_multi_paragraph },
    { "selection_extract_text_reports_size_on_overflow", test_selection_extract_text_reports_size_on_overflow },
    { "selection_delete_single_paragraph", test_selection_delete_single_paragraph },
    { "selection_delete_empty_selection_is_noop", test_selection_delete_empty_selection_is_noop },
    { "selection_delete_multi_paragraph_then_undo_then_redo", test_selection_delete_multi_paragraph_then_undo_then_redo },
    { "selection_delete_out_pos_lands_at_delete_start", test_selection_delete_out_pos_lands_at_delete_start },
    { "selection_delete_swallows_whole_middle_paragraph", test_selection_delete_swallows_whole_middle_paragraph },
    { "selection_delete_no_leaks", test_selection_delete_no_leaks },

    { "motion_word_forward_skips_word_then_blank", test_motion_word_forward_skips_word_then_blank },
    { "motion_word_forward_stops_at_punctuation_boundary", test_motion_word_forward_stops_at_punctuation_boundary },
    { "motion_word_forward_big_word_merges_punct", test_motion_word_forward_big_word_merges_punct },
    { "motion_word_backward_returns_to_word_starts", test_motion_word_backward_returns_to_word_starts },
    { "motion_word_end_lands_after_last_char_of_word", test_motion_word_end_lands_after_last_char_of_word },
    { "motion_word_forward_crosses_paragraph_boundary", test_motion_word_forward_crosses_paragraph_boundary },
    { "motion_line_start_end_first_nonblank", test_motion_line_start_end_first_nonblank },
    { "motion_line_first_nonblank_on_all_blank_line_lands_at_end", test_motion_line_first_nonblank_on_all_blank_line_lands_at_end },
    { "motion_paragraph_down_up", test_motion_paragraph_down_up },
    { "motion_para_block_forward_backward_finds_empty_paragraphs", test_motion_para_block_forward_backward_finds_empty_paragraphs },

    { "vimode_insert_then_escape", test_vimode_insert_then_escape },
    { "vimode_hjkl_and_word_motion", test_vimode_hjkl_and_word_motion },
    { "vimode_count_prefix_motion", test_vimode_count_prefix_motion },
    { "vimode_dw_deletes_word", test_vimode_dw_deletes_word },
    { "vimode_dw_does_not_eat_paragraph_break", test_vimode_dw_does_not_eat_paragraph_break },
    { "vimode_dd_deletes_whole_line", test_vimode_dd_deletes_whole_line },
    { "vimode_count_dd_deletes_multiple_lines", test_vimode_count_dd_deletes_multiple_lines },
    { "vimode_d_count_motion_deletes_multiple_lines", test_vimode_d_count_motion_deletes_multiple_lines },
    { "vimode_count_dd_undoes_as_one_step", test_vimode_count_dd_undoes_as_one_step },
    { "vimode_dollar_deletes_to_end_of_line", test_vimode_dollar_deletes_to_end_of_line },
    { "vimode_dd_on_last_paragraph_merges_upward", test_vimode_dd_on_last_paragraph_merges_upward },
    { "vimode_yy_p_duplicates_line_below", test_vimode_yy_p_duplicates_line_below },
    { "vimode_cw_change_word", test_vimode_cw_change_word },
    { "vimode_insert_session_undoes_as_one_step", test_vimode_insert_session_undoes_as_one_step },
    { "vimode_cw_undoes_as_one_step", test_vimode_cw_undoes_as_one_step },
    { "vimode_visual_mode_delete", test_vimode_visual_mode_delete },
    { "vimode_visual_line_mode_delete_spans_paragraphs", test_vimode_visual_line_mode_delete_spans_paragraphs },
    { "vimode_ex_command_save_quit", test_vimode_ex_command_save_quit },
    { "vimode_ex_save_with_filename", test_vimode_ex_save_with_filename },
    { "vimode_ex_open_command", test_vimode_ex_open_command },
    { "vimode_ex_filename_edge_cases", test_vimode_ex_filename_edge_cases },
    { "vimode_search_forward", test_vimode_search_forward },
    { "vimode_search_backward", test_vimode_search_backward },
    { "vimode_n_and_capital_n_respect_search_direction", test_vimode_n_and_capital_n_respect_search_direction },
    { "vimode_substitute_current_paragraph_replaces_first_match_only", test_vimode_substitute_current_paragraph_replaces_first_match_only },
    { "vimode_substitute_global_flag_replaces_every_match_in_paragraph", test_vimode_substitute_global_flag_replaces_every_match_in_paragraph },
    { "vimode_substitute_percent_scopes_to_whole_section", test_vimode_substitute_percent_scopes_to_whole_section },
    { "vimode_substitute_percent_and_global_replaces_everywhere", test_vimode_substitute_percent_and_global_replaces_everywhere },
    { "vimode_substitute_ignore_case_flag", test_vimode_substitute_ignore_case_flag },
    { "vimode_substitute_confirm_y_replaces_n_skips", test_vimode_substitute_confirm_y_replaces_n_skips },
    { "vimode_substitute_confirm_a_replaces_all_remaining", test_vimode_substitute_confirm_a_replaces_all_remaining },
    { "vimode_substitute_confirm_q_stops_without_replacing", test_vimode_substitute_confirm_q_stops_without_replacing },
    { "vimode_substitute_malformed_command_falls_through_to_beep", test_vimode_substitute_malformed_command_falls_through_to_beep },
    { "vimode_substitute_global_undoes_as_one_step", test_vimode_substitute_global_undoes_as_one_step },
    { "vimode_o_opens_line_below", test_vimode_o_opens_line_below },
    { "vimode_x_deletes_char_forward", test_vimode_x_deletes_char_forward },
    { "vimode_ctrl_chord_is_not_bare_letter", test_vimode_ctrl_chord_is_not_bare_letter },
    { "vimode_alt_chord_is_not_bare_letter", test_vimode_alt_chord_is_not_bare_letter },
    { "vimode_no_leaks", test_vimode_no_leaks },

    { "wordstar_diamond_char_motions", test_wordstar_diamond_char_motions },
    { "wordstar_diamond_word_motions", test_wordstar_diamond_word_motions },
    { "wordstar_diamond_paragraph_motions", test_wordstar_diamond_paragraph_motions },
    { "wordstar_arrow_keys_also_move", test_wordstar_arrow_keys_also_move },
    { "wordstar_plain_typing_is_non_modal", test_wordstar_plain_typing_is_non_modal },
    { "wordstar_ctrl_g_deletes_char_forward", test_wordstar_ctrl_g_deletes_char_forward },
    { "wordstar_ctrl_t_deletes_word_forward", test_wordstar_ctrl_t_deletes_word_forward },
    { "wordstar_ctrl_y_deletes_current_paragraph", test_wordstar_ctrl_y_deletes_current_paragraph },
    { "wordstar_block_copy_appends_at_cursor", test_wordstar_block_copy_appends_at_cursor },
    { "wordstar_block_move_cursor_before_block", test_wordstar_block_move_cursor_before_block },
    { "wordstar_block_move_cursor_after_block", test_wordstar_block_move_cursor_after_block },
    { "wordstar_block_move_refuses_when_cursor_inside_block", test_wordstar_block_move_refuses_when_cursor_inside_block },
    { "wordstar_block_delete", test_wordstar_block_delete },
    { "wordstar_kx_returns_save_quit_and_kq_returns_quit", test_wordstar_kx_returns_save_quit_and_kq_returns_quit },
    { "wordstar_unbound_prefix_subkey_beeps", test_wordstar_unbound_prefix_subkey_beeps },
    { "wordstar_format_prefix_returns_correct_kinds", test_wordstar_format_prefix_returns_correct_kinds },
    { "wordstar_find_jumps_to_match", test_wordstar_find_jumps_to_match },
    { "wordstar_find_not_found_beeps", test_wordstar_find_not_found_beeps },
    { "wordstar_find_empty_text_beeps", test_wordstar_find_empty_text_beeps },
    { "wordstar_find_escape_cancels_prompt", test_wordstar_find_escape_cancels_prompt },
    { "wordstar_find_backward_option", test_wordstar_find_backward_option },
    { "wordstar_find_ignore_case_and_whole_word_options", test_wordstar_find_ignore_case_and_whole_word_options },
    { "wordstar_replace_confirm_y_and_n", test_wordstar_replace_confirm_y_and_n },
    { "wordstar_replace_no_confirm_option_replaces_silently", test_wordstar_replace_no_confirm_option_replaces_silently },
    { "wordstar_replace_global_option_restarts_from_document_start", test_wordstar_replace_global_option_restarts_from_document_start },
    { "wordstar_ctrl_l_repeats_last_find", test_wordstar_ctrl_l_repeats_last_find },
    { "wordstar_ctrl_l_without_prior_search_beeps", test_wordstar_ctrl_l_without_prior_search_beeps },
    { "wordstar_no_leaks", test_wordstar_no_leaks },

    { "emacs_basic_char_and_line_motion", test_emacs_basic_char_and_line_motion },
    { "emacs_line_start_and_end", test_emacs_line_start_and_end },
    { "emacs_meta_word_motion_via_alt_bit", test_emacs_meta_word_motion_via_alt_bit },
    { "emacs_meta_via_escape_prefix_fallback", test_emacs_meta_via_escape_prefix_fallback },
    { "emacs_meta_doc_start_and_end", test_emacs_meta_doc_start_and_end },
    { "emacs_plain_typing_is_non_modal", test_emacs_plain_typing_is_non_modal },
    { "emacs_ctrl_d_and_backspace_delete", test_emacs_ctrl_d_and_backspace_delete },
    { "emacs_meta_d_kills_word_forward_and_ctrl_y_yanks", test_emacs_meta_d_kills_word_forward_and_ctrl_y_yanks },
    { "emacs_meta_backspace_kills_word_backward", test_emacs_meta_backspace_kills_word_backward },
    { "emacs_ctrl_k_kills_to_line_end_then_merges_paragraph", test_emacs_ctrl_k_kills_to_line_end_then_merges_paragraph },
    { "emacs_mark_and_kill_region", test_emacs_mark_and_kill_region },
    { "emacs_kill_region_without_mark_beeps", test_emacs_kill_region_without_mark_beeps },
    { "emacs_meta_w_copies_region_without_moving_cursor", test_emacs_meta_w_copies_region_without_moving_cursor },
    { "emacs_exchange_point_and_mark", test_emacs_exchange_point_and_mark },
    { "emacs_ctrl_x_prefix_save_open_quit", test_emacs_ctrl_x_prefix_save_open_quit },
    { "emacs_ctrl_x_u_undoes", test_emacs_ctrl_x_u_undoes },
    { "emacs_ctrl_c_prefix_format_bindings", test_emacs_ctrl_c_prefix_format_bindings },
    { "emacs_ctrl_g_cancels_pending_prefix", test_emacs_ctrl_g_cancels_pending_prefix },
    { "emacs_unbound_chord_beeps", test_emacs_unbound_chord_beeps },
    { "emacs_isearch_forward_lands_at_match_end", test_emacs_isearch_forward_lands_at_match_end },
    { "emacs_isearch_backward_lands_at_match_start", test_emacs_isearch_backward_lands_at_match_start },
    { "emacs_isearch_ctrl_s_repeats_forward", test_emacs_isearch_ctrl_s_repeats_forward },
    { "emacs_isearch_backspace_shrinks_pattern_and_researches", test_emacs_isearch_backspace_shrinks_pattern_and_researches },
    { "emacs_isearch_failing_search_does_not_move_cursor", test_emacs_isearch_failing_search_does_not_move_cursor },
    { "emacs_isearch_ctrl_g_aborts_to_anchor", test_emacs_isearch_ctrl_g_aborts_to_anchor },
    { "emacs_query_replace_confirm_vocabulary", test_emacs_query_replace_confirm_vocabulary },
    { "emacs_query_replace_bang_replaces_all_remaining", test_emacs_query_replace_bang_replaces_all_remaining },
    { "emacs_query_replace_not_found_beeps", test_emacs_query_replace_not_found_beeps },
    { "emacs_query_replace_escape_cancels_prompt", test_emacs_query_replace_escape_cancels_prompt },
    { "emacs_no_leaks", test_emacs_no_leaks },

    { "wordperfect_arrow_char_motion", test_wordperfect_arrow_char_motion },
    { "wordperfect_ctrl_arrow_word_motion", test_wordperfect_ctrl_arrow_word_motion },
    { "wordperfect_up_down_paragraph_motion", test_wordperfect_up_down_paragraph_motion },
    { "wordperfect_home_end", test_wordperfect_home_end },
    { "wordperfect_ctrl_home_goes_to_doc_start", test_wordperfect_ctrl_home_goes_to_doc_start },
    { "wordperfect_plain_typing_is_non_modal", test_wordperfect_plain_typing_is_non_modal },
    { "wordperfect_delete_forward_and_backward", test_wordperfect_delete_forward_and_backward },
    { "wordperfect_ctrl_backspace_deletes_word_backward", test_wordperfect_ctrl_backspace_deletes_word_backward },
    { "wordperfect_ctrl_end_deletes_to_line_end", test_wordperfect_ctrl_end_deletes_to_line_end },
    { "wordperfect_f1_undelete_is_single_level_undo", test_wordperfect_f1_undelete_is_single_level_undo },
    { "wordperfect_f6_f8_f7_f10_return_correct_kinds", test_wordperfect_f6_f8_f7_f10_return_correct_kinds },
    { "wordperfect_block_toggle_extends_by_motion", test_wordperfect_block_toggle_extends_by_motion },
    { "wordperfect_move_block_then_retrieve", test_wordperfect_move_block_then_retrieve },
    { "wordperfect_ctrl_f4_without_block_beeps", test_wordperfect_ctrl_f4_without_block_beeps },
    { "wordperfect_shift_f10_with_empty_register_beeps", test_wordperfect_shift_f10_with_empty_register_beeps },
    { "wordperfect_search_forward_jumps_to_match", test_wordperfect_search_forward_jumps_to_match },
    { "wordperfect_search_backward", test_wordperfect_search_backward },
    { "wordperfect_search_empty_text_beeps", test_wordperfect_search_empty_text_beeps },
    { "wordperfect_search_not_found_beeps", test_wordperfect_search_not_found_beeps },
    { "wordperfect_search_escape_cancels_prompt", test_wordperfect_search_escape_cancels_prompt },
    { "wordperfect_search_f2_twice_repeats_last_search", test_wordperfect_search_f2_twice_repeats_last_search },
    { "wordperfect_replace_confirm_y_and_n", test_wordperfect_replace_confirm_y_and_n },
    { "wordperfect_replace_no_confirm_replaces_silently", test_wordperfect_replace_no_confirm_replaces_silently },
    { "wordperfect_no_leaks", test_wordperfect_no_leaks },

    { "msword_arrow_char_motion", test_msword_arrow_char_motion },
    { "msword_shift_arrow_extends_selection", test_msword_shift_arrow_extends_selection },
    { "msword_ctrl_arrow_word_motion_and_shift_ctrl_extends_by_word", test_msword_ctrl_arrow_word_motion_and_shift_ctrl_extends_by_word },
    { "msword_home_end_and_ctrl_home_end", test_msword_home_end_and_ctrl_home_end },
    { "msword_plain_typing_is_non_modal", test_msword_plain_typing_is_non_modal },
    { "msword_typing_over_selection_replaces_it", test_msword_typing_over_selection_replaces_it },
    { "msword_delete_and_backspace_without_selection", test_msword_delete_and_backspace_without_selection },
    { "msword_delete_with_active_selection_deletes_the_selection", test_msword_delete_with_active_selection_deletes_the_selection },
    { "msword_ctrl_delete_and_backspace_delete_whole_words", test_msword_ctrl_delete_and_backspace_delete_whole_words },
    { "msword_enter_with_selection_deletes_it_first", test_msword_enter_with_selection_deletes_it_first },
    { "msword_ctrl_a_selects_all", test_msword_ctrl_a_selects_all },
    { "msword_ctrl_z_undo_and_ctrl_y_redo", test_msword_ctrl_z_undo_and_ctrl_y_redo },
    { "msword_ctrl_letter_action_bindings", test_msword_ctrl_letter_action_bindings },
    { "msword_unbound_ctrl_letter_beeps", test_msword_unbound_ctrl_letter_beeps },
    { "msword_ctrl_q_quits", test_msword_ctrl_q_quits },
    { "msword_find_jumps_to_match", test_msword_find_jumps_to_match },
    { "msword_find_wraps_around_document", test_msword_find_wraps_around_document },
    { "msword_find_not_found_beeps", test_msword_find_not_found_beeps },
    { "msword_find_escape_cancels_prompt", test_msword_find_escape_cancels_prompt },
    { "msword_shift_f4_repeats_last_find", test_msword_shift_f4_repeats_last_find },
    { "msword_shift_f4_without_prior_find_beeps", test_msword_shift_f4_without_prior_find_beeps },
    { "msword_replace_r_and_n", test_msword_replace_r_and_n },
    { "msword_replace_all_replaces_silently", test_msword_replace_all_replaces_silently },
    { "msword_replace_confirm_escape_closes", test_msword_replace_confirm_escape_closes },
    { "msword_no_leaks", test_msword_no_leaks },

    { "actions_find_by_id", test_actions_find_by_id },
    { "rtf_image_exports_a_real_pict", test_rtf_image_exports_a_real_pict },
    { "rtf_image_round_trips_through_export_and_import", test_rtf_image_round_trips_through_export_and_import },
    { "rtf_image_import_without_a_sink_skips_the_picture", test_rtf_image_import_without_a_sink_skips_the_picture },
    { "rtf_image_import_keeps_the_text_around_a_bad_picture", test_rtf_image_import_keeps_the_text_around_a_bad_picture },
    { "rtf_image_dib_header_is_well_formed", test_rtf_image_dib_header_is_well_formed },
    { "rtf_image_rows_are_bottom_up_and_padded", test_rtf_image_rows_are_bottom_up_and_padded },
    { "rtf_image_without_a_source_is_an_honest_placeholder", test_rtf_image_without_a_source_is_an_honest_placeholder },
    { "rtf_image_export_is_not_an_empty_paragraph", test_rtf_image_export_is_not_an_empty_paragraph },
    { "rtf_image_missing_cache_file_still_writes_the_promised_size", test_rtf_image_missing_cache_file_still_writes_the_promised_size },
    { "rtf_image_text_paragraphs_are_unaffected", test_rtf_image_text_paragraphs_are_unaffected },
    { "actions_bold_styles_the_selection", test_actions_bold_styles_the_selection },
    { "actions_bold_toggles_input_style_with_no_selection", test_actions_bold_toggles_input_style_with_no_selection },
    { "actions_bold_on_an_all_bold_selection_clears_it", test_actions_bold_on_an_all_bold_selection_clears_it },
    { "actions_find_by_shortcut", test_actions_find_by_shortcut },
    { "actions_every_shortcut_is_unique", test_actions_every_shortcut_is_unique },
    { "actions_table_row_order_matches_enum", test_actions_table_row_order_matches_enum },
    { "actions_every_action_is_reachable", test_actions_every_action_is_reachable },
    { "actions_every_action_has_a_label", test_actions_every_action_has_a_label },
    { "actions_format_bold_toggles_style_flags", test_actions_format_bold_toggles_style_flags },
    { "actions_file_quit_sets_flag", test_actions_file_quit_sets_flag },
    { "actions_edit_copy_sets_clipboard", test_actions_edit_copy_sets_clipboard },
    { "actions_format_size_uses_dialog_choice", test_actions_format_size_uses_dialog_choice },
    { "actions_cut_then_paste_gives_the_document_back", test_actions_cut_then_paste_gives_the_document_back },
    { "actions_cut_takes_every_paragraph_of_an_opened_file", test_actions_cut_takes_every_paragraph_of_an_opened_file },
    { "actions_cut_copes_with_a_run_of_no_text", test_actions_cut_copes_with_a_run_of_no_text },
    { "actions_cut_is_one_undo_step", test_actions_cut_is_one_undo_step },
    { "actions_cut_keeps_a_document_the_clipboard_cannot_hold", test_actions_cut_keeps_a_document_the_clipboard_cannot_hold },
    { "viewport_counts_display_lines_across_paragraphs", test_viewport_counts_display_lines_across_paragraphs },
    { "viewport_follows_the_cursor_with_the_least_scroll", test_viewport_follows_the_cursor_with_the_least_scroll },
    { "viewport_follow_keeps_the_top_inside_a_shorter_document", test_viewport_follow_keeps_the_top_inside_a_shorter_document },
    { "viewport_page_moves_view_and_cursor_together", test_viewport_page_moves_view_and_cursor_together },
    { "viewport_page_lands_mid_paragraph_on_a_wrapped_line", test_viewport_page_lands_mid_paragraph_on_a_wrapped_line },
    { "viewport_scroll_by_hand_holds_until_a_keystroke", test_viewport_scroll_by_hand_holds_until_a_keystroke },
    { "viewport_page_keys_work_under_every_scheme", test_viewport_page_keys_work_under_every_scheme },
    { "viewport_replace_document_goes_back_to_the_top", test_viewport_replace_document_goes_back_to_the_top },
    { "viewport_finds_the_line_under_y_and_the_position_under_a_click", test_viewport_finds_the_line_under_y_and_the_position_under_a_click },
    { "viewport_click_moves_or_extends_under_every_scheme", test_viewport_click_moves_or_extends_under_every_scheme },
    { "tools_grammar_check_reports_locally_without_a_gateway", test_tools_grammar_check_reports_locally_without_a_gateway },
    { "tools_grammar_and_chat_want_a_paragraph_with_text", test_tools_grammar_and_chat_want_a_paragraph_with_text },
    { "tools_gateway_tools_say_when_there_is_no_device", test_tools_gateway_tools_say_when_there_is_no_device },
    { "tools_spell_check_names_the_first_unknown_word", test_tools_spell_check_names_the_first_unknown_word },
    { "format_md_headings_become_big_bold_paragraphs", test_format_md_headings_become_big_bold_paragraphs },
    { "format_md_emphasis_makes_styled_runs", test_format_md_emphasis_makes_styled_runs },
    { "format_md_joins_wrapped_lines_and_splits_blocks", test_format_md_joins_wrapped_lines_and_splits_blocks },
    { "format_md_leaves_text_that_only_looks_like_markup", test_format_md_leaves_text_that_only_looks_like_markup },
    { "format_md_nested_and_adjacent_emphasis", test_format_md_nested_and_adjacent_emphasis },
    { "format_md_export_round_trips_what_the_model_holds", test_format_md_export_round_trips_what_the_model_holds },
    { "format_md_export_of_a_plain_document_is_its_paragraphs", test_format_md_export_of_a_plain_document_is_its_paragraphs },
    { "actions_copy_takes_only_the_selection", test_actions_copy_takes_only_the_selection },
    { "actions_cut_takes_only_the_selection", test_actions_cut_takes_only_the_selection },
    { "actions_paste_replaces_the_selection", test_actions_paste_replaces_the_selection },
    { "actions_copy_and_cut_with_nothing_selected_do_nothing", test_actions_copy_and_cut_with_nothing_selected_do_nothing },
    { "actions_cut_in_visual_line_takes_whole_paragraphs", test_actions_cut_in_visual_line_takes_whole_paragraphs },
    { "actions_save_file_picks_the_format_by_extension_and_remembers", test_actions_save_file_picks_the_format_by_extension_and_remembers },
    { "actions_save_goes_to_the_remembered_name_without_asking", test_actions_save_goes_to_the_remembered_name_without_asking },
    { "actions_load_file_reads_text_and_rtf_and_keeps_the_name", test_actions_load_file_reads_text_and_rtf_and_keeps_the_name },
    { "actions_odt_and_pdf_go_through_the_gateway", test_actions_odt_and_pdf_go_through_the_gateway },
    { "actions_file_print_routes", test_actions_file_print_routes },
    { "actions_file_print_options", test_actions_file_print_options },
    { "actions_file_print_choose_printer", test_actions_file_print_choose_printer },
    { "config_printer_keys", test_config_printer_keys },
    { "pageprint_pages", test_pageprint_pages },
    { "pageprint_printable_area", test_pageprint_printable_area },
    { "config_save_printer", test_config_save_printer },
    { "actions_file_open_asks_picks_and_a_new_document_forgets_the_name", test_actions_file_open_asks_picks_and_a_new_document_forgets_the_name },
    { "actions_paragraph_buttons_change_the_caret_paragraph", test_actions_paragraph_buttons_change_the_caret_paragraph },
    { "actions_toggle_tab_adds_snaps_and_removes", test_actions_toggle_tab_adds_snaps_and_removes },
    { "rulerbar_hit_test_finds_buttons_gaps_and_the_ruler", test_rulerbar_hit_test_finds_buttons_gaps_and_the_ruler },
    { "rulerbar_signature_follows_what_the_bar_shows", test_rulerbar_signature_follows_what_the_bar_shows },
    { "actions_page_break_splits_and_undo_merges", test_actions_page_break_splits_and_undo_merges },
    { "actions_header_prompt_types_edits_and_sets", test_actions_header_prompt_types_edits_and_sets },
    { "actions_document_name_is_the_last_path_component", test_actions_document_name_is_the_last_path_component },
    { "selspan_marks_the_lines_a_selection_crosses", test_selspan_marks_the_lines_a_selection_crosses },
    { "selspan_within_one_line", test_selspan_within_one_line },
    { "selspan_starting_at_a_line_end_shows_nothing_there", test_selspan_starting_at_a_line_end_shows_nothing_there },
    { "actions_paste_inserts_at_the_cursor", test_actions_paste_inserts_at_the_cursor },
    { "actions_request_quit_asks_when_modified", test_actions_request_quit_asks_when_modified },
    { "actions_save_quit_stays_when_the_save_did_not_land", test_actions_save_quit_stays_when_the_save_did_not_land },
    { "actions_replace_document_resets_the_journal", test_actions_replace_document_resets_the_journal },
    { "actions_write_file_marks_saved_only_when_it_lands", test_actions_write_file_marks_saved_only_when_it_lands },
    { "actions_align_marks_the_document_modified", test_actions_align_marks_the_document_modified },
    { "actions_cut_with_an_image_paragraph", test_actions_cut_with_an_image_paragraph },

    { "statusline_paint_does_not_crash_with_various_views", test_statusline_paint_does_not_crash_with_various_views },

    { "config_defaults_to_vi_on_empty_input", test_config_defaults_to_vi_on_empty_input },
    { "config_parses_keymap_vim", test_config_parses_keymap_vim },
    { "config_parses_keymap_wordstar", test_config_parses_keymap_wordstar },
    { "config_parses_keymap_emacs", test_config_parses_keymap_emacs },
    { "config_parses_keymap_wordperfect", test_config_parses_keymap_wordperfect },
    { "config_parses_keymap_msword", test_config_parses_keymap_msword },
    { "config_key_and_value_are_case_insensitive", test_config_key_and_value_are_case_insensitive },
    { "config_tolerates_surrounding_whitespace", test_config_tolerates_surrounding_whitespace },
    { "config_skips_comment_and_blank_lines", test_config_skips_comment_and_blank_lines },
    { "config_unrecognized_value_falls_back_to_default", test_config_unrecognized_value_falls_back_to_default },
    { "config_unrecognized_key_is_ignored", test_config_unrecognized_key_is_ignored },
    { "config_malformed_line_without_equals_is_skipped", test_config_malformed_line_without_equals_is_skipped },
    { "config_tolerates_crlf_line_endings", test_config_tolerates_crlf_line_endings },
    { "config_last_line_without_trailing_newline_is_parsed", test_config_last_line_without_trailing_newline_is_parsed },
    { "config_load_missing_file_yields_default", test_config_load_missing_file_yields_default },
    { "config_shipped_sample_parses", test_config_shipped_sample_parses },
    { "config_sample_inert_keys_do_not_alter_defaults", test_config_sample_inert_keys_do_not_alter_defaults },

    { "keymap_app_ctx_size_stays_under_ceiling", test_keymap_app_ctx_size_stays_under_ceiling },
    { "keymap_init_selects_vi_and_seeds_cursor_at_doc_start", test_keymap_init_selects_vi_and_seeds_cursor_at_doc_start },
    { "keymap_feed_key_moves_cursor_through_vi_arm", test_keymap_feed_key_moves_cursor_through_vi_arm },
    { "keymap_feed_key_insert_reports_inserted_char", test_keymap_feed_key_insert_reports_inserted_char },
    { "keymap_feed_key_quit_reports_quit_kind", test_keymap_feed_key_quit_reports_quit_kind },
    { "keymap_ex_filename_reaches_outcome", test_keymap_ex_filename_reaches_outcome },
    { "keymap_get_status_view_reports_vi_mode_label", test_keymap_get_status_view_reports_vi_mode_label },

    { "help_doc_parses_single_node", test_help_doc_parses_single_node },
    { "help_doc_node_without_title_falls_back_to_name", test_help_doc_node_without_title_falls_back_to_name },
    { "help_doc_parses_link_line", test_help_doc_parses_link_line },
    { "help_doc_braces_without_bar_are_plain_text", test_help_doc_braces_without_bar_are_plain_text },
    { "help_doc_find_node_by_name", test_help_doc_find_node_by_name },
    { "help_doc_missing_endnode_at_eof_implicitly_closes", test_help_doc_missing_endnode_at_eof_implicitly_closes },
    { "help_doc_skips_content_before_first_node", test_help_doc_skips_content_before_first_node },
    { "help_doc_blank_line_preserved_as_empty_text_line", test_help_doc_blank_line_preserved_as_empty_text_line },
    { "help_doc_empty_source_yields_zero_nodes", test_help_doc_empty_source_yields_zero_nodes },
    { "help_doc_no_leaks", test_help_doc_no_leaks },

    { "help_wrap_line_greedy_word_wrap_hand_computed", test_help_wrap_line_greedy_word_wrap_hand_computed },
    { "help_wrap_line_overlong_word_gets_its_own_row", test_help_wrap_line_overlong_word_gets_its_own_row },
    { "help_wrap_line_empty_text_yields_zero_rows", test_help_wrap_line_empty_text_yields_zero_rows },
    { "help_wrap_line_respects_max_rows_cap", test_help_wrap_line_respects_max_rows_cap },
    { "help_view_init_clamps_start_node", test_help_view_init_clamps_start_node },
    { "help_view_escape_and_q_request_close", test_help_view_escape_and_q_request_close },
    { "help_view_link_selection_clamped_at_bounds", test_help_view_link_selection_clamped_at_bounds },
    { "help_view_enter_follows_link_and_backspace_goes_back", test_help_view_enter_follows_link_and_backspace_goes_back },
    { "help_view_enter_on_broken_link_target_is_a_noop", test_help_view_enter_on_broken_link_target_is_a_noop },
    { "help_view_page_scroll_clamped_at_zero", test_help_view_page_scroll_clamped_at_zero },
    { "help_view_paint_does_not_crash_with_various_states", test_help_view_paint_does_not_crash_with_various_states },
    { "help_view_no_leaks", test_help_view_no_leaks },

    { "help_content_retrowp_help_parses_and_all_links_resolve", test_help_content_retrowp_help_parses_and_all_links_resolve },

    { "layout_greedy_wrap_hand_computed_fixture", test_layout_greedy_wrap_hand_computed_fixture },
    { "layout_geometry_indent_and_alignment_place_the_line", test_layout_geometry_indent_and_alignment_place_the_line },
    { "layout_geometry_later_lines_use_the_left_indent_only", test_layout_geometry_later_lines_use_the_left_indent_only },
    { "layout_geometry_spacing_shapes_the_line_boxes", test_layout_geometry_spacing_shapes_the_line_boxes },
    { "layout_geometry_default_tabs_advance_to_half_inches", test_layout_geometry_default_tabs_advance_to_half_inches },
    { "layout_geometry_a_tab_can_start_a_line", test_layout_geometry_a_tab_can_start_a_line },
    { "layout_geometry_right_and_centre_stops", test_layout_geometry_right_and_centre_stops },
    { "layout_geometry_fragments_caret_and_clicks_agree_on_tabs", test_layout_geometry_fragments_caret_and_clicks_agree_on_tabs },
    { "layout_geometry_styles_with_different_tabs_stay_apart", test_layout_geometry_styles_with_different_tabs_stay_apart },
    { "layout_geometry_page_break_before_starts_a_page", test_layout_geometry_page_break_before_starts_a_page },
    { "layout_single_word_wider_than_column_overflows", test_layout_single_word_wider_than_column_overflows },
    { "layout_empty_paragraph_gets_one_zero_width_line", test_layout_empty_paragraph_gets_one_zero_width_line },
    { "layout_word_spanning_run_boundary_is_one_token", test_layout_word_spanning_run_boundary_is_one_token },
    { "layout_editing_one_paragraph_does_not_remeasure_whole_document", test_layout_editing_one_paragraph_does_not_remeasure_whole_document },

    { "justify_non_justify_align_returns_zero", test_justify_non_justify_align_returns_zero },
    { "justify_pos_at_x_finds_the_nearest_boundary", test_justify_pos_at_x_finds_the_nearest_boundary },
    { "justify_last_line_of_paragraph_returns_zero", test_justify_last_line_of_paragraph_returns_zero },
    { "justify_computes_extra_per_gap", test_justify_computes_extra_per_gap },
    { "justify_single_word_line_has_no_gap", test_justify_single_word_line_has_no_gap },
    { "justify_fragment_iter_splits_at_run_boundary_not_at_word", test_justify_fragment_iter_splits_at_run_boundary_not_at_word },
    { "justify_cursor_x_measures_from_line_start_not_paragraph_start", test_justify_cursor_x_measures_from_line_start_not_paragraph_start },
    { "justify_cursor_x_past_line_end_clamps_to_line_width", test_justify_cursor_x_past_line_end_clamps_to_line_width },
    { "justify_cursor_x_accumulates_justify_slack", test_justify_cursor_x_accumulates_justify_slack },
    { "justify_cursor_x_crosses_run_boundaries", test_justify_cursor_x_crosses_run_boundaries },
    { "justify_cursor_x_in_skipped_zero_length_run", test_justify_cursor_x_in_skipped_zero_length_run },

    { "paginate_known_page_size_yields_expected_page_count", test_paginate_known_page_size_yields_expected_page_count },
    { "paginate_new_section_always_starts_new_page", test_paginate_new_section_always_starts_new_page },
    { "paginate_no_leaks", test_paginate_no_leaks },

    { "undo_insert_then_undo_then_redo", test_undo_insert_then_undo_then_redo },
    { "style_range_bolds_a_word_in_the_middle", test_style_range_bolds_a_word_in_the_middle },
    { "style_range_splits_the_run_it_partially_covers", test_style_range_splits_the_run_it_partially_covers },
    { "style_range_marks_the_paragraph_for_relayout", test_style_range_marks_the_paragraph_for_relayout },
    { "style_range_marks_the_document_modified", test_style_range_marks_the_document_modified },
    { "style_range_undo_restores_every_run_then_redo_reapplies", test_style_range_undo_restores_every_run_then_redo_reapplies },
    { "style_range_undo_of_a_partial_range_restores_both_fragments", test_style_range_undo_of_a_partial_range_restores_both_fragments },
    { "style_range_keeps_each_runs_own_size", test_style_range_keeps_each_runs_own_size },
    { "style_range_clear_turns_a_flag_back_off", test_style_range_clear_turns_a_flag_back_off },
    { "style_range_accepts_its_ends_in_either_order", test_style_range_accepts_its_ends_in_either_order },
    { "style_range_empty_range_changes_nothing", test_style_range_empty_range_changes_nothing },
    { "style_range_spans_paragraphs", test_style_range_spans_paragraphs },
    { "range_has_flags_answers_the_toggle_question", test_range_has_flags_answers_the_toggle_question },
    { "undo_delete_all_cases_then_undo_redo", test_undo_delete_all_cases_then_undo_redo },
    { "undo_new_edit_clears_redo", test_undo_new_edit_clears_redo },
    { "undo_empty_journal_returns_error", test_undo_empty_journal_returns_error },
    { "undo_no_leaks", test_undo_no_leaks },
    { "ws_soft_return_joins_hard_return_splits", test_ws_soft_return_joins_hard_return_splits },
    { "ws_soft_return_does_not_double_space", test_ws_soft_return_does_not_double_space },
    { "ws_high_bit_is_stripped", test_ws_high_bit_is_stripped },
    { "ws_style_toggles_produce_styled_runs", test_ws_style_toggles_produce_styled_runs },
    { "ws_dot_commands_dropped_but_prose_kept", test_ws_dot_commands_dropped_but_prose_kept },
    { "ws_blank_lines_survive", test_ws_blank_lines_survive },
    { "ws_sniff_rejects_plain_text", test_ws_sniff_rejects_plain_text },
    { "ws_garbage_still_imports", test_ws_garbage_still_imports },
    { "ws_import_is_not_modified", test_ws_import_is_not_modified },
    { "document_import_is_not_modified", test_document_import_is_not_modified },
    { "journaled_edit_marks_document_modified", test_journaled_edit_marks_document_modified },
    { "failed_undo_does_not_mark_modified", test_failed_undo_does_not_mark_modified },
    { "real_undo_marks_modified", test_real_undo_marks_modified },
    { "undo_fuzz_n_ops_then_n_undos_restores_original", test_undo_fuzz_n_ops_then_n_undos_restores_original },
    { "undo_split_paragraph_then_undo_then_redo", test_undo_split_paragraph_then_undo_then_redo },
    { "undo_merge_paragraph_then_undo_then_redo", test_undo_merge_paragraph_then_undo_then_redo },
    { "undo_split_merge_no_leaks", test_undo_split_merge_no_leaks },

    { "strpool_reserves_offset_zero", test_strpool_reserves_offset_zero },
    { "strpool_multiple_appends_survive_growth", test_strpool_multiple_appends_survive_growth },
    { "strpool_empty_append", test_strpool_empty_append },

    { "docmodel_init_free_has_no_leaks", test_docmodel_init_free_has_no_leaks },
    { "docmodel_init_sets_usable_defaults", test_docmodel_init_sets_usable_defaults },
    { "docmodel_cstyle_add_survives_growth", test_docmodel_cstyle_add_survives_growth },
    { "docmodel_cstyle_find_or_add_reuses_identical_style", test_docmodel_cstyle_find_or_add_reuses_identical_style },
    { "docmodel_pstyle_find_or_add_reuses_identical_style", test_docmodel_pstyle_find_or_add_reuses_identical_style },
    { "docmodel_build_known_tree", test_docmodel_build_known_tree },
    { "docmodel_pstyle_and_cstyle_set", test_docmodel_pstyle_and_cstyle_set },

    { "image_add_get_roundtrip", test_image_add_get_roundtrip },
    { "image_merge_absorbing_a_picture_undoes_to_the_picture", test_image_merge_absorbing_a_picture_undoes_to_the_picture },
    { "image_merge_text_onto_a_picture_keeps_the_text", test_image_merge_text_onto_a_picture_keeps_the_text },
    { "image_undoing_enter_on_a_picture_keeps_the_picture", test_image_undoing_enter_on_a_picture_keeps_the_picture },
    { "image_add_survives_growth", test_image_add_survives_growth },
    { "edit_insert_image_splits_paragraph", test_edit_insert_image_splits_paragraph },
    { "edit_insert_image_undo_redo", test_edit_insert_image_undo_redo },

    { "image_layout_relayout_produces_one_line_from_descriptor", test_image_layout_relayout_produces_one_line_from_descriptor },
    { "image_layout_missing_handle_degrades_to_zero_size", test_image_layout_missing_handle_degrades_to_zero_size },
    { "image_layout_fragment_iter_returns_false_immediately", test_image_layout_fragment_iter_returns_false_immediately },
    { "image_layout_justify_extra_is_zero", test_image_layout_justify_extra_is_zero },
    { "image_layout_paginate_mixed_text_and_image", test_image_layout_paginate_mixed_text_and_image },

    { "format_txt_roundtrip_simple", test_format_txt_roundtrip_simple },
    { "format_txt_roundtrip_empty", test_format_txt_roundtrip_empty },
    { "format_txt_roundtrip_multi_para", test_format_txt_roundtrip_multi_para },
    { "format_txt_roundtrip_blank_lines", test_format_txt_roundtrip_blank_lines },
    { "format_txt_roundtrip_unicode", test_format_txt_roundtrip_unicode },
    { "format_txt_import_no_trailing_newline", test_format_txt_import_no_trailing_newline },
    { "format_txt_import_crlf", test_format_txt_import_crlf },
    { "format_txt_import_empty_creates_zero_paragraphs", test_format_txt_import_empty_creates_zero_paragraphs },
    { "format_txt_export_reports_size_on_overflow", test_format_txt_export_reports_size_on_overflow },
    { "format_prn_letter_page", test_format_prn_letter_page },
    { "format_prn_crlf", test_format_prn_crlf },
    { "format_prn_wraps_at_words", test_format_prn_wraps_at_words },
    { "format_prn_cuts_a_long_word", test_format_prn_cuts_a_long_word },
    { "format_prn_alignment", test_format_prn_alignment },
    { "format_prn_indents_and_tabs", test_format_prn_indents_and_tabs },
    { "format_prn_spacing", test_format_prn_spacing },
    { "format_prn_pages_header_footer", test_format_prn_pages_header_footer },
    { "format_prn_page_break_and_top_space", test_format_prn_page_break_and_top_space },
    { "format_prn_encodings", test_format_prn_encodings },
    { "format_prn_empty_document", test_format_prn_empty_document },
    { "format_prn_epson", test_format_prn_epson },
    { "format_prn_pcl", test_format_prn_pcl },
    { "format_prn_pcl_paper", test_format_prn_pcl_paper },
    { "format_prn_amiga", test_format_prn_amiga },
    { "format_prn_postscript", test_format_prn_postscript },
    { "format_prn_driver_empty", test_format_prn_driver_empty },
    { "format_prn_postscript_file", test_format_prn_postscript_file },

    { "format_rtf_import_rejects_bad_header", test_format_rtf_import_rejects_bad_header },
    { "format_rtf_import_decodes_other_code_pages", test_format_rtf_import_decodes_other_code_pages },
    { "fonts_table_finds_by_name_ignoring_case", test_fonts_table_finds_by_name_ignoring_case },
    { "fonts_family_guess_from_a_bare_name", test_fonts_family_guess_from_a_bare_name },
    { "fonts_rtf_word_font_table_and_charsets", test_fonts_rtf_word_font_table_and_charsets },
    { "fonts_rtf_default_font_and_plain", test_fonts_rtf_default_font_and_plain },
    { "fonts_rtf_round_trip_keeps_faces_and_the_default", test_fonts_rtf_round_trip_keeps_faces_and_the_default },
    { "fonts_face_pick_by_name_then_family", test_fonts_face_pick_by_name_then_family },
    { "fonts_family_bits_match_the_seam", test_fonts_family_bits_match_the_seam },
    { "actions_font_restyles_the_selection_as_one_step", test_actions_font_restyles_the_selection_as_one_step },
    { "actions_typed_text_keeps_its_neighbours_style_unless_armed", test_actions_typed_text_keeps_its_neighbours_style_unless_armed },
    { "rulerbar_has_font_and_size", test_rulerbar_has_font_and_size },
    { "convert_pdf_export_picks_faces_by_family", test_convert_pdf_export_picks_faces_by_family },
    { "rulerbar_panel_look_follows_the_colour_count", test_rulerbar_panel_look_follows_the_colour_count },
    { "rulerbar_dragging_indents", test_rulerbar_dragging_indents },
    { "rulerbar_dragging_tabs", test_rulerbar_dragging_tabs },
    { "convert_odt_carries_headers_footers_and_the_page", test_convert_odt_carries_headers_footers_and_the_page },
    { "convert_a_reply_in_parts_arrives_whole", test_convert_a_reply_in_parts_arrives_whole },
    { "rtf_modern_sections_in", test_rtf_modern_sections_in },
    { "rtf_modern_sections_round_trip", test_rtf_modern_sections_round_trip },
    { "convert_sections_reach_the_pdf_and_odt", test_convert_sections_reach_the_pdf_and_odt },
    { "actions_header_follows_the_caret_section", test_actions_header_follows_the_caret_section },
    { "actions_page_setup_paper_and_margins", test_actions_page_setup_paper_and_margins },
    { "actions_section_break_and_backspace", test_actions_section_break_and_backspace },
    { "actions_delete_across_a_section_break", test_actions_delete_across_a_section_break },
    { "convert_a_large_document_goes_in_parts", test_convert_a_large_document_goes_in_parts },
    { "rtf_modern_unicode_and_its_fallback", test_rtf_modern_unicode_and_its_fallback },
    { "rtf_modern_plain_resets_and_hidden_text_stays_out", test_rtf_modern_plain_resets_and_hidden_text_stays_out },
    { "rtf_modern_word_noise_stays_out_of_the_body", test_rtf_modern_word_noise_stays_out_of_the_body },
    { "rtf_modern_table_rows_become_tabbed_paragraphs", test_rtf_modern_table_rows_become_tabbed_paragraphs },
    { "rtf_modern_line_spacing_tabs_and_page_setup", test_rtf_modern_line_spacing_tabs_and_page_setup },
    { "rtf_modern_alike_paragraphs_share_one_style", test_rtf_modern_alike_paragraphs_share_one_style },
    { "rtf_modern_export_round_trips_the_new_properties", test_rtf_modern_export_round_trips_the_new_properties },
    { "rtf_modern_page_breaks_both_ways", test_rtf_modern_page_breaks_both_ways },
    { "rtf_modern_headers_and_footers_both_ways", test_rtf_modern_headers_and_footers_both_ways },
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
    { "offload_capabilities_advertise_chat_but_not_convert_import", test_offload_capabilities_advertise_chat_but_not_convert_import },
    { "offload_unsupported_op_reports_unsupported", test_offload_unsupported_op_reports_unsupported },
    { "offload_capabilities_advertise_image", test_offload_capabilities_advertise_image },
    { "offload_image_returns_canned_fixture_bitmap", test_offload_image_returns_canned_fixture_bitmap },
    { "offload_capabilities_advertise_dictate", test_offload_capabilities_advertise_dictate },
    { "offload_dictate_returns_canned_transcript", test_offload_dictate_returns_canned_transcript },
    { "offload_dictate_rejects_malformed_request", test_offload_dictate_rejects_malformed_request },
    { "offload_corrupt_response_is_rejected", test_offload_corrupt_response_is_rejected },
    { "offload_timeout_on_delayed_gateway", test_offload_timeout_on_delayed_gateway },
    { "offload_gateway_dies_before_responding", test_offload_gateway_dies_before_responding },
    { "offload_short_frame_times_out", test_offload_short_frame_times_out },
    { "offload_submit_fails_after_gateway_killed", test_offload_submit_fails_after_gateway_killed },
    { "convert_odt_import_via_gateway", test_convert_odt_import_via_gateway },
    { "convert_print_needs_a_printer", test_convert_print_needs_a_printer },
    { "convert_print_to_a_folder", test_convert_print_to_a_folder },
    { "convert_print_through_lp", test_convert_print_through_lp },
    { "convert_hello_mid_session_is_a_new_handshake", test_convert_hello_mid_session_is_a_new_handshake },
    { "convert_gateway_resyncs_past_noise", test_convert_gateway_resyncs_past_noise },
    { "convert_request_without_hello", test_convert_request_without_hello },
    { "convert_print_options_listed", test_convert_print_options_listed },
    { "convert_print_option_refused", test_convert_print_option_refused },
    { "convert_odt_export_roundtrip_via_gateway", test_convert_odt_export_roundtrip_via_gateway },
    { "convert_pdf_export_via_gateway", test_convert_pdf_export_via_gateway },
    { "convert_pdf_export_uses_the_document_page", test_convert_pdf_export_uses_the_document_page },
    { "convert_pdf_export_honours_page_breaks", test_convert_pdf_export_honours_page_breaks },
    { "convert_unsupported_format_id_reports_unsupported", test_convert_unsupported_format_id_reports_unsupported },
    { "convert_malformed_payload_is_error", test_convert_malformed_payload_is_error },

    { "image_convert_real_pillow_color_round_trip", test_image_convert_real_pillow_color_round_trip },
    { "image_convert_real_pillow_grayscale_round_trip", test_image_convert_real_pillow_grayscale_round_trip },
    { "image_convert_pillow_unavailable_reports_unsupported", test_image_convert_pillow_unavailable_reports_unsupported },
    { "image_convert_malformed_request_is_error", test_image_convert_malformed_request_is_error },

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
    { "grammar_realistic_paragraph_finds_every_planted_issue", test_grammar_realistic_paragraph_finds_every_planted_issue },

    { "search_find_forward_basic", test_search_find_forward_basic },
    { "search_find_forward_not_found", test_search_find_forward_not_found },
    { "search_find_forward_wraps_around_document", test_search_find_forward_wraps_around_document },
    { "search_find_backward_basic", test_search_find_backward_basic },
    { "search_find_backward_wraps_around_document", test_search_find_backward_wraps_around_document },
    { "search_find_backward_picks_nearest_preceding_match", test_search_find_backward_picks_nearest_preceding_match },
    { "search_ignore_case_matches_different_case", test_search_ignore_case_matches_different_case },
    { "search_whole_word_rejects_substring_match", test_search_whole_word_rejects_substring_match },
    { "search_whole_word_false_matches_substring", test_search_whole_word_false_matches_substring },
    { "search_match_crosses_run_boundary", test_search_match_crosses_run_boundary },
    { "search_match_selection_within_single_run", test_search_match_selection_within_single_run },
    { "search_empty_needle_is_error", test_search_empty_needle_is_error }
};

int main(void)
{
    int count = (int)(sizeof(g_cases) / sizeof(g_cases[0]));
    return test_run_all(g_cases, count);
}
