package org.slate.editor;

import android.app.*;
import android.content.*;
import android.graphics.Color;
import android.view.*;
import android.widget.*;
import org.json.*;
import java.util.Locale;

final class Appearance {
    private static final String[] KEYS = {"background", "surface", "text", "muted", "accent", "code"};
    private static final String[] LABELS = {"Document background", "Panels", "Text", "Secondary text", "Accent", "Code"};
    private static final String[][] DEFAULTS = {
        {"#fbfbfb", "#efefef", "#252525", "#626262", "#484848", "#555555"},
        {"#101010", "#191919", "#e7e7e7", "#a6a6a6", "#c8c8c8", "#d2d2d2"}
    };
    private final SharedPreferences store;
    boolean dark;
    Appearance(Context context) {
        store = context.getSharedPreferences("appearance", Context.MODE_PRIVATE);
        dark = store.getBoolean("dark", true);
    }

    private String value(boolean night, int i) {
        return store.getString((night ? "night." : "day.") + KEYS[i], DEFAULTS[night ? 1 : 0][i]);
    }

    int background() { return Color.parseColor(value(dark, 0)); }
    int surface() { return Color.parseColor(value(dark, 1)); }
    int text() { return Color.parseColor(value(dark, 2)); }
    int muted() { return Color.parseColor(value(dark, 3)); }
    int accent() { return Color.parseColor(value(dark, 4)); }

    JSONObject json() {
        JSONObject out = new JSONObject();
        try {
            out.put("dark", dark);
            for (int i = 0; i < KEYS.length; i++) out.put(KEYS[i], value(dark, i));
            out.put("border", value(dark, 3));
        } catch (JSONException e) { throw new IllegalStateException(e); }
        return out;
    }

    int dialogTheme() { return dark ? android.R.style.Theme_Material_Dialog_Alert : android.R.style.Theme_Material_Light_Dialog_Alert; }

    void edit(Activity activity, Runnable apply) {
        AlertDialog.Builder builder = new AlertDialog.Builder(activity, dialogTheme());
        Context context = builder.getContext();
        LinearLayout fields = new LinearLayout(context);
        fields.setOrientation(LinearLayout.VERTICAL);
        int pad = (int)(18 * context.getResources().getDisplayMetrics().density);
        fields.setPadding(pad, 0, pad, 0);
        RadioGroup mode = new RadioGroup(context);
        mode.setOrientation(LinearLayout.HORIZONTAL);
        RadioButton night = new RadioButton(context), day = new RadioButton(context);
        night.setId(View.generateViewId());
        day.setId(View.generateViewId());
        night.setText("Dark");
        day.setText("Light");
        mode.addView(night);
        mode.addView(day);
        fields.addView(mode);
        EditText[] inputs = new EditText[KEYS.length];
        String[][] draft = new String[2][KEYS.length];
        for (int i = 0; i < KEYS.length; i++) {
            draft[0][i] = value(false, i);
            draft[1][i] = value(true, i);
            TextView label = new TextView(context);
            label.setText(LABELS[i]);
            fields.addView(label);
            EditText input = new EditText(context);
            input.setSingleLine();
            input.setContentDescription(LABELS[i]);
            inputs[i] = input;
            fields.addView(input);
        }
        boolean[] selected = {dark};
        Runnable fill = () -> {
            for (int i = 0; i < inputs.length; i++) inputs[i].setText(draft[selected[0] ? 1 : 0][i]);
        };
        fill.run();
        mode.check(dark ? night.getId() : day.getId());
        mode.setOnCheckedChangeListener((g, id) -> {
            for (int i = 0; i < inputs.length; i++) draft[selected[0] ? 1 : 0][i] = inputs[i].getText().toString();
            selected[0] = id == night.getId();
            fill.run();
        });
        ScrollView scroll = new ScrollView(context);
        scroll.addView(fields);
        AlertDialog dialog = builder.setTitle("Appearance").setView(scroll)
            .setPositiveButton("Save", null).setNegativeButton("Cancel", null).create();
        dialog.setOnShowListener(ignored -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            SharedPreferences.Editor editor = store.edit();
            for (int i = 0; i < inputs.length; i++) draft[selected[0] ? 1 : 0][i] = inputs[i].getText().toString();
            for (int m = 0; m <= 1; m++) for (int i = 0; i < KEYS.length; i++) {
                if (!draft[m][i].matches("#[a-fA-F0-9]{6}")) {
                    mode.check(m == 1 ? night.getId() : day.getId());
                    inputs[i].setError("Use #RRGGBB");
                    inputs[i].requestFocus();
                    return;
                }
                editor.putString((m == 1 ? "night." : "day.") + KEYS[i], draft[m][i].toLowerCase(Locale.ROOT));
            }
            dark = selected[0];
            editor.putBoolean("dark", dark).apply();
            dialog.dismiss();
            apply.run();
        }));
        dialog.show();
    }
}
