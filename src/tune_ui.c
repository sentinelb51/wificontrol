/* tune_ui.c -- the tuning dialog.
 *
 * One row per setting the system says has a fixed set of choices, one dropdown
 * showing exactly those choices, and an Apply button.  The app never proposes
 * a value and never remembers a previous one; to undo something you pick the
 * other entry in the same dropdown.
 */
#define WIN32_LEAN_AND_MEAN
#include "tune.h"
#include "wlan_win32.h"
#include "resource.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    wc_guid  adapter;
    wchar_t  adapter_name[WC_NAME_MAX];
    tune_list *list;
} tune_ctx;

static void set_row_value(HWND lv, int row, const tune_setting *s)
{
    wchar_t text[TUNE_TEXT_MAX + 16];
    const wchar_t *label = (s->sel >= 0) ? s->opt[s->sel].label : L"(not one of the choices)";
    /* A pending change is marked, not stored: it lives in the dropdown only. */
    _snwprintf(text, TUNE_TEXT_MAX + 16, L"%s%s",
               (s->sel != s->cur) ? L"→ " : L"", label);
    text[TUNE_TEXT_MAX + 15] = L'\0';
    ListView_SetItemText(lv, row, 2, text);
}

static void fill_rows(HWND dlg, tune_ctx *c)
{
    HWND lv = GetDlgItem(dlg, IDC_TUNE_LIST);
    int keep = ListView_GetNextItem(lv, -1, LVNI_SELECTED);

    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);

    for (int i = 0; i < c->list->n; ++i) {
        tune_setting *s = &c->list->s[i];
        LVITEMW it = { .mask = LVIF_TEXT | LVIF_PARAM, .iItem = i,
                       .pszText = s->group, .lParam = i };
        int row = ListView_InsertItem(lv, &it);
        if (row < 0) continue;
        ListView_SetItemText(lv, row, 1, s->name);
        set_row_value(lv, row, s);
    }
    if (keep >= 0 && keep < c->list->n)
        ListView_SetItemState(lv, keep, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
}

static void collect(tune_ctx *c)
{
    memset(c->list, 0, sizeof *c->list);
    tune_collect_driver(c->list, &c->adapter);
    tune_collect_power(c->list);
}

static int selected_index(HWND dlg)
{
    HWND lv = GetDlgItem(dlg, IDC_TUNE_LIST);
    int row = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (row < 0) return -1;
    LVITEMW it = { .mask = LVIF_PARAM, .iItem = row };
    if (!ListView_GetItem(lv, &it)) return -1;
    return (int)it.lParam;
}

static void load_combo(HWND dlg, tune_ctx *c)
{
    HWND cb = GetDlgItem(dlg, IDC_TUNE_VAL);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);

    int i = selected_index(dlg);
    if (i < 0 || i >= c->list->n) { EnableWindow(cb, FALSE); return; }

    const tune_setting *s = &c->list->s[i];
    for (int k = 0; k < s->n_opt; ++k)
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)s->opt[k].label);
    SendMessageW(cb, CB_SETCURSEL, (WPARAM)(s->sel >= 0 ? s->sel : -1), 0);
    EnableWindow(cb, TRUE);

    SetDlgItemTextW(dlg, IDC_TUNE_HINT,
        s->src == TUNE_DRIVER
            ? L"Adapter properties are read by the driver when it starts, so a change "
              L"applies after the adapter restarts or the machine reboots."
            : L"Power scheme changes apply as soon as they are written.");
}

static void do_apply(HWND dlg, tune_ctx *c)
{
    unsigned long err = 0;
    int n = tune_apply(c->list, &err);

    wchar_t msg[512], detail[256];
    if (err) {
        wcw_format_error(err, detail, 256);
        _snwprintf(msg, 512, L"Wrote %d setting%s. At least one failed: %s",
                   n, n == 1 ? L"" : L"s", detail);
    } else if (n == 0) {
        wcscpy(msg, L"Nothing to write — no dropdown was changed.");
    } else {
        _snwprintf(msg, 512, L"Wrote %d setting%s.", n, n == 1 ? L"" : L"s");
    }
    msg[511] = L'\0';

    /* Re-read so the list shows what the system actually holds now. */
    collect(c);
    fill_rows(dlg, c);
    load_combo(dlg, c);
    MessageBoxW(dlg, msg, L"Tuning", err ? MB_ICONWARNING : MB_ICONINFORMATION);
}

static void do_restart(HWND dlg, tune_ctx *c)
{
    if (MessageBoxW(dlg,
            L"Restart the Wi-Fi adapter now?\n\n"
            L"This disables and re-enables the device so the driver re-reads its "
            L"properties. The connection drops for a few seconds.",
            L"Restart adapter", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;

    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    EnableWindow(GetDlgItem(dlg, IDC_TUNE_RESTART), FALSE);
    EnableWindow(GetDlgItem(dlg, IDC_TUNE_APPLY), FALSE);

    unsigned long e = tune_restart_adapter(c->list);

    EnableWindow(GetDlgItem(dlg, IDC_TUNE_RESTART), TRUE);
    EnableWindow(GetDlgItem(dlg, IDC_TUNE_APPLY), TRUE);
    SetCursor(old);

    if (e != ERROR_SUCCESS) {
        wchar_t detail[256], msg[512];
        wcw_format_error(e, detail, 256);
        _snwprintf(msg, 512, L"Could not restart the adapter: %s\n\n"
                             L"If it is now disabled, re-enable it in Device Manager.",
                   detail);
        msg[511] = L'\0';
        MessageBoxW(dlg, msg, L"Restart adapter", MB_ICONWARNING);
    }
    collect(c);
    fill_rows(dlg, c);
    load_combo(dlg, c);
}

static INT_PTR CALLBACK tune_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    tune_ctx *c = (tune_ctx *)GetWindowLongPtrW(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG: {
        c = (tune_ctx *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)c);

        HWND lv = GetDlgItem(dlg, IDC_TUNE_LIST);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        static const wchar_t *cols[3] = { L"Group", L"Setting", L"Value" };
        static const int widths[3] = { 150, 210, 200 };
        for (int i = 0; i < 3; ++i) {
            LVCOLUMNW col = { .mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM,
                              .iSubItem = i, .pszText = (LPWSTR)cols[i], .cx = widths[i] };
            ListView_InsertColumn(lv, i, &col);
        }

        wchar_t head[256];
        _snwprintf(head, 256, L"Adapter properties for %s, and the active power scheme.",
                   c->adapter_name[0] ? c->adapter_name : L"this adapter");
        head[255] = L'\0';
        SetDlgItemTextW(dlg, IDC_TUNE_FOR, head);

        collect(c);
        fill_rows(dlg, c);
        if (c->list->n > 0) ListView_SetItemState(lv, 0, LVIS_SELECTED | LVIS_FOCUSED,
                                                  LVIS_SELECTED | LVIS_FOCUSED);
        load_combo(dlg, c);

        if (!c->list->have_instance)
            SetDlgItemTextW(dlg, IDC_TUNE_HINT,
                L"Could not locate this adapter's driver key, so only power settings "
                L"are listed.");
        return TRUE;
    }

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDC_TUNE_LIST && nh->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *nv = (NMLISTVIEW *)lp;
            if ((nv->uChanged & LVIF_STATE) && (nv->uNewState & LVIS_SELECTED))
                load_combo(dlg, c);
        }
        return FALSE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_TUNE_VAL:
            if (HIWORD(wp) == CBN_SELCHANGE && c) {
                int i = selected_index(dlg);
                int k = (int)SendMessageW(GetDlgItem(dlg, IDC_TUNE_VAL), CB_GETCURSEL, 0, 0);
                if (i >= 0 && i < c->list->n && k >= 0) {
                    c->list->s[i].sel = k;
                    HWND lv = GetDlgItem(dlg, IDC_TUNE_LIST);
                    set_row_value(lv, ListView_GetNextItem(lv, -1, LVNI_SELECTED),
                                  &c->list->s[i]);
                }
            }
            return TRUE;
        case IDC_TUNE_APPLY:   if (c) do_apply(dlg, c);   return TRUE;
        case IDC_TUNE_RESTART: if (c) do_restart(dlg, c); return TRUE;
        case IDOK:
        case IDCANCEL:         EndDialog(dlg, 0);         return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

void tune_dialog(HWND parent, const wc_guid *adapter, const wchar_t *adapter_name)
{
    tune_ctx c = { .adapter = *adapter };
    wcsncpy(c.adapter_name, adapter_name ? adapter_name : L"", WC_NAME_MAX - 1);
    c.adapter_name[WC_NAME_MAX - 1] = L'\0';

    /* Well over a megabyte with the option tables: not a stack object. */
    c.list = malloc(sizeof *c.list);
    if (!c.list) return;

    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_TUNE),
                    parent, tune_proc, (LPARAM)&c);
    free(c.list);
}
