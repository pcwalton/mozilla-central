%filter substitution

%define font_xlarge 5.08mozmm
%define font_xnormal 2.75mozmm
%define font_normal 2.54mozmm
%define font_snormal 2.33mozmm
%define font_small 1.91mozmm
%define font_xsmall 1.69mozmm
%define font_tiny 1.48mozmm
%define font_xtiny 1.27mozmm

%define touch_row 7.41mozmm
%define touch_button_xlarge 7.62mozmm
%define touch_button_large 6.77mozmm
%define touch_button_small 5.93mozmm
%define touch_action_minwidth 21.17mozmm
%define touch_normal 6.77mozmm

%define margin_context_popup 3.39mozmm
%define margin_large 2.54mozmm
%define margin_xxxnormal 1.69mozmm
%define margin_xnormal 1.06mozmm
%define margin_normal 0.85mozmm
%define margin_snormal 0.64mozmm
%define margin_small 0.42mozmm
%define margin_tiny 0.21mozmm
%define margin_xtiny 0.11mozmm

%define padding_xlarge 3.39mozmm
%define padding_large 2.54mozmm
%define padding_xxxnormal 1.69mozmm
%define padding_xxnormal 1.27mozmm
%define padding_xnormal 1.06mozmm
%define padding_normal 0.85mozmm
%define padding_snormal 0.64mozmm
%define padding_small 0.42mozmm
%define padding_xsmall 0.21mozmm
%define padding_tiny 0.11mozmm

%define border_width_xxlarge 0.64mozmm
%define border_width_xlarge 0.42mozmm
%define border_width_large 0.32mozmm
%define border_width_small 0.21mozmm
%define border_width_tiny 0.11mozmm

%define border_radius_normal 0.85mozmm
%define border_radius_small 0.64mozmm
%define border_radius_tiny 0.21mozmm

%define shadow_width_xlarge 1.06mozmm
%define shadow_width_large 0.64mozmm
%define shadow_width_small 0.21mozmm

%define textbox_height 5.08mozmm

%define dropmarker_padding 0.53mozmm

%define progressmeter_height 3.39mozmm

%define urlbar_edit_height 6.35mozmm
%define urlbar_edit_indent 0.85mozmm
%define urlbar_max_width   96mozmm

%define scroller_thickness 0.64mozmm
%define scroller_minimum 1.27mozmm

%define sidebar_width_minimum 8.47mozmm
%define sidebar_button_height 7.41mozmm
%define documenttab_margin_bottom 0.53mozmm

%define placelabel_padding 8.47mozmm
%define placeitem_padding 4.23mozmm

%define autocomplete_item_container_image_padding 0.53mozmm
%define autocomplete_item_container_position 0.21mozmm
%define autocomplete_item_container_size 2.75mozmm
%define autocomplete_item_container_padding 5.08mozmm

%define autocomplete_item_subtitle_margin 2.75mozmm
%define autocomplete_item_label_margin 3.18mozmm
%define autocomplete_item_tags_margin 3.39mozmm

%define autocompleteresult_padding 0.53mozmm

%define dialog_width 76.2mozmm

%define appmenu_portrait_height 21.17mozmm
%define appmenu_button_height  10.48mozmm

%define tablet_panel_controls  40mozmm
%define tablet_panel_minwidth  124mozmm

%ifdef MOZ_PLATFORM_MAEMO
%define orientation -moz-device-orientation
%elifdef ANDROID
%define orientation -moz-device-orientation
%else
%define orientation orientation
%endif
