#include "PresetManagerDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "Tab.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dataview.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/srchctrl.h>
#include <wx/stattext.h>

namespace Slic3r { namespace GUI {

namespace {
enum Column { colVisible = 0, colName, colKind, colBase };

wxString kind_label(const PresetBundle &bundle, const Preset &preset)
{
    if (preset.is_system)
        return bundle.has_system_preset_override(preset.type, preset.name) ? _L("Stock (edited)") : _L("Stock");
    if (preset.is_project_embedded)
        return _L("Project");
    if (preset.is_from_bundle())
        return _L("Bundle");
    return _L("User");
}

wxString base_label(const Preset &preset)
{
    if (preset.vendor != nullptr && !preset.vendor->name.empty())
        return from_u8(preset.vendor->name);
    return from_u8(preset.inherits());
}
} // namespace

PresetManagerDialog::PresetManagerDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Preset Manager"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);
    auto *sizer = new wxBoxSizer(wxVERTICAL);

    auto *intro = new wxStaticText(this, wxID_ANY,
        _L("Untick a preset to hide it from the preset lists. The selected preset stays visible until you pick another one.\n"
           "To edit a stock preset: select it in the sidebar, change its settings, then press \"Save edits into stock preset\" here. "
           "User presets based on it pick up the change after a restart."));
    intro->Wrap(FromDIP(720));
    sizer->Add(intro, 0, wxEXPAND | wxALL, FromDIP(10));

    auto *notebook = new wxNotebook(this, wxID_ANY);
    m_pages.resize(3);
    m_pages[0].type = Preset::TYPE_PRINTER;
    m_pages[1].type = Preset::TYPE_FILAMENT;
    m_pages[2].type = Preset::TYPE_PRINT;
    build_page(notebook, m_pages[0], _L("Printers"));
    build_page(notebook, m_pages[1], _L("Filaments"));
    build_page(notebook, m_pages[2], _L("Processes"));
    sizer->Add(notebook, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    auto *close = new wxButton(this, wxID_CLOSE, _L("Close"));
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CLOSE); });
    SetEscapeId(wxID_CLOSE);
    sizer->Add(close, 0, wxALIGN_RIGHT | wxALL, FromDIP(10));

    SetSizerAndFit(sizer);
    SetSize(FromDIP(wxSize(780, 640)));
    SetMinSize(FromDIP(wxSize(560, 420)));
    CenterOnParent();

    for (Page &page : m_pages)
        fill_page(page);
}

void PresetManagerDialog::build_page(wxNotebook *notebook, Page &page, const wxString &title)
{
    auto *panel = new wxPanel(notebook);
    auto *sizer = new wxBoxSizer(wxVERTICAL);

    auto *filter_row = new wxBoxSizer(wxHORIZONTAL);
    page.search = new wxSearchCtrl(panel, wxID_ANY);
    page.search->ShowCancelButton(true);
    page.search->SetDescriptiveText(_L("Search"));
    filter_row->Add(page.search, 1, wxEXPAND | wxRIGHT, FromDIP(8));
    page.stock_only = new wxCheckBox(panel, wxID_ANY, _L("Stock presets only"));
    filter_row->Add(page.stock_only, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(filter_row, 0, wxEXPAND | wxALL, FromDIP(8));

    page.list = new wxDataViewListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_ROW_LINES | wxDV_SINGLE);
    page.list->AppendToggleColumn(_L("Visible"), wxDATAVIEW_CELL_ACTIVATABLE, FromDIP(60));
    page.list->AppendTextColumn(_L("Name"), wxDATAVIEW_CELL_INERT, FromDIP(360), wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
    page.list->AppendTextColumn(_L("Kind"), wxDATAVIEW_CELL_INERT, FromDIP(110), wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
    page.list->AppendTextColumn(_L("Vendor / based on"), wxDATAVIEW_CELL_INERT, FromDIP(200), wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
    sizer->Add(page.list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    auto *visibility_row = new wxBoxSizer(wxHORIZONTAL);
    auto *show_all = new wxButton(panel, wxID_ANY, _L("Show all"));
    auto *hide_stock = new wxButton(panel, wxID_ANY, _L("Hide all stock"));
    page.reset_stock = new wxButton(panel, wxID_ANY, _L("Reset selected row to stock"));
    visibility_row->Add(show_all, 0, wxRIGHT, FromDIP(6));
    visibility_row->Add(hide_stock, 0, wxRIGHT, FromDIP(6));
    visibility_row->AddStretchSpacer();
    visibility_row->Add(page.reset_stock, 0);
    sizer->Add(visibility_row, 0, wxEXPAND | wxALL, FromDIP(8));

    auto *edit_row = new wxBoxSizer(wxHORIZONTAL);
    page.selected_label = new wxStaticText(panel, wxID_ANY, wxEmptyString);
    page.save_edits = new wxButton(panel, wxID_ANY, _L("Save edits into stock preset"));
    edit_row->Add(page.selected_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    edit_row->Add(page.save_edits, 0);
    sizer->Add(edit_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    panel->SetSizer(sizer);
    notebook->AddPage(panel, title);

    const size_t page_idx = &page - m_pages.data();
    page.search->Bind(wxEVT_TEXT, [this, page_idx](wxCommandEvent &) { fill_page(m_pages[page_idx]); });
    page.search->Bind(wxEVT_SEARCHCTRL_CANCEL_BTN, [this, page_idx](wxCommandEvent &) {
        m_pages[page_idx].search->Clear();
        fill_page(m_pages[page_idx]);
    });
    page.stock_only->Bind(wxEVT_CHECKBOX, [this, page_idx](wxCommandEvent &) { fill_page(m_pages[page_idx]); });
    page.list->Bind(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, [this, page_idx](wxDataViewEvent &evt) { on_toggle(m_pages[page_idx], evt); });
    page.list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this, page_idx](wxDataViewEvent &) { update_buttons(m_pages[page_idx]); });
    show_all->Bind(wxEVT_BUTTON, [this, page_idx](wxCommandEvent &) { set_all(m_pages[page_idx], false); });
    hide_stock->Bind(wxEVT_BUTTON, [this, page_idx](wxCommandEvent &) { set_all(m_pages[page_idx], true); });
    page.reset_stock->Bind(wxEVT_BUTTON, [this, page_idx](wxCommandEvent &) { reset_selected(m_pages[page_idx]); });
    page.save_edits->Bind(wxEVT_BUTTON, [this, page_idx](wxCommandEvent &) { save_edits(m_pages[page_idx]); });
}

void PresetManagerDialog::fill_page(Page &page)
{
    PresetBundle     *bundle     = wxGetApp().preset_bundle;
    AppConfig        *config     = wxGetApp().app_config;
    PresetCollection *collection = bundle->collection_for_type(page.type);
    if (collection == nullptr)
        return;

    const std::string filter     = boost::algorithm::to_lower_copy(into_u8(page.search->GetValue()));
    const bool        stock_only = page.stock_only->GetValue();

    m_filling = true;
    page.list->Freeze();
    page.list->DeleteAllItems();
    page.row_names.clear();
    for (const Preset &preset : *collection) {
        if (preset.is_default)
            continue;
        const bool hidden = PresetBundle::is_preset_hidden(*config, page.type, preset.name);
        // Presets the Configuration Wizard left out (printers of uninstalled models, filaments
        // for them) are not listed; the wizard manages those.
        if (!preset.is_visible && !hidden)
            continue;
        if (stock_only && !preset.is_system)
            continue;
        if (!filter.empty() && boost::algorithm::to_lower_copy(preset.name).find(filter) == std::string::npos)
            continue;
        wxVector<wxVariant> row;
        row.push_back(wxVariant(!hidden));
        row.push_back(wxVariant(from_u8(preset.name)));
        row.push_back(wxVariant(kind_label(*bundle, preset)));
        row.push_back(wxVariant(base_label(preset)));
        page.list->AppendItem(row);
        page.row_names.push_back(preset.name);
    }
    page.list->Thaw();
    m_filling = false;
    update_buttons(page);
}

void PresetManagerDialog::update_buttons(Page &page)
{
    PresetBundle     *bundle     = wxGetApp().preset_bundle;
    PresetCollection *collection = bundle->collection_for_type(page.type);
    if (collection == nullptr)
        return;

    const Preset &selected = collection->get_selected_preset();
    const bool    dirty    = collection->current_is_dirty();
    wxString      text;
    if (!selected.is_system)
        text = wxString::Format(_L("Selected: %s (not a stock preset)"), from_u8(selected.name));
    else if (dirty)
        text = wxString::Format(_L("Selected: %s (has unsaved edits)"), from_u8(selected.name));
    else
        text = wxString::Format(_L("Selected: %s (no unsaved edits)"), from_u8(selected.name));
    page.selected_label->SetLabel(text);
    page.save_edits->Enable(selected.is_system && dirty);

    bool can_reset = false;
    const int row = page.list->GetSelectedRow();
    if (row != wxNOT_FOUND && size_t(row) < page.row_names.size())
        can_reset = bundle->has_system_preset_override(page.type, page.row_names[row]);
    page.reset_stock->Enable(can_reset);
}

void PresetManagerDialog::on_toggle(Page &page, wxDataViewEvent &evt)
{
    if (m_filling || evt.GetColumn() != colVisible)
        return;
    const int row = page.list->ItemToRow(evt.GetItem());
    if (row == wxNOT_FOUND || size_t(row) >= page.row_names.size())
        return;
    const bool visible = page.list->GetToggleValue(row, colVisible);
    set_hidden(page, page.row_names[row], !visible);
    wxGetApp().app_config->save();
    refresh_app(page.type, false);
}

void PresetManagerDialog::set_hidden(Page &page, const std::string &name, bool hidden)
{
    AppConfig        *config     = wxGetApp().app_config;
    PresetCollection *collection = wxGetApp().preset_bundle->collection_for_type(page.type);
    const std::string key        = PresetBundle::hidden_preset_key(page.type, name);
    if (hidden)
        config->set(PresetBundle::HIDDEN_PRESETS_SECTION, key, "1");
    else
        config->erase(PresetBundle::HIDDEN_PRESETS_SECTION, key);

    if (Preset *preset = collection->find_preset(name, false); preset != nullptr && preset->name == name) {
        const bool selected = collection->get_selected_preset().name == name;
        preset->is_visible  = !hidden || selected;
    }
}

void PresetManagerDialog::set_all(Page &page, bool hidden)
{
    PresetCollection *collection = wxGetApp().preset_bundle->collection_for_type(page.type);
    AppConfig        *config     = wxGetApp().app_config;
    std::vector<std::string> names;
    for (const Preset &preset : *collection) {
        if (preset.is_default)
            continue;
        const bool is_hidden = PresetBundle::is_preset_hidden(*config, page.type, preset.name);
        if (hidden) {
            if (preset.is_system && preset.is_visible && !is_hidden)
                names.push_back(preset.name);
        } else if (is_hidden) {
            names.push_back(preset.name);
        }
    }
    for (const std::string &name : names)
        set_hidden(page, name, hidden);
    config->save();
    fill_page(page);
    refresh_app(page.type, false);
}

void PresetManagerDialog::save_edits(Page &page)
{
    PresetBundle     *bundle     = wxGetApp().preset_bundle;
    PresetCollection *collection = bundle->collection_for_type(page.type);
    const std::string name       = collection->get_selected_preset().name;

    MessageDialog confirm(this,
        wxString::Format(_L("Save the current edits into the stock preset \"%s\"?\n\n"
                            "The changes are kept in a separate file, so you can undo them later with \"Reset selected row to stock\". "
                            "A vendor profile update keeps your changes."), from_u8(name)),
        _L("Preset Manager"), wxYES_NO | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES)
        return;

    const std::string error = bundle->save_system_preset_override(page.type);
    if (!error.empty()) {
        MessageDialog(this, from_u8(error), _L("Preset Manager"), wxOK | wxICON_ERROR).ShowModal();
        return;
    }
    refresh_app(page.type, true);
    fill_page(page);
}

void PresetManagerDialog::reset_selected(Page &page)
{
    const int row = page.list->GetSelectedRow();
    if (row == wxNOT_FOUND || size_t(row) >= page.row_names.size())
        return;
    const std::string name = page.row_names[row];

    MessageDialog confirm(this, wxString::Format(_L("Restore the vendor values of \"%s\"? Your edits to this stock preset are deleted."), from_u8(name)),
                          _L("Preset Manager"), wxYES_NO | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES)
        return;

    PresetBundle     *bundle     = wxGetApp().preset_bundle;
    PresetCollection *collection = bundle->collection_for_type(page.type);
    const bool        selected   = collection->get_selected_preset().name == name;
    if (selected && collection->current_is_dirty()) {
        MessageDialog(this, _L("The selected preset has unsaved edits. Save or discard them first."), _L("Preset Manager"), wxOK | wxICON_WARNING).ShowModal();
        return;
    }
    const std::string error = bundle->reset_system_preset_override(page.type, name);
    if (!error.empty()) {
        MessageDialog(this, from_u8(error), _L("Preset Manager"), wxOK | wxICON_ERROR).ShowModal();
        return;
    }
    refresh_app(page.type, selected);
    fill_page(page);
}

void PresetManagerDialog::refresh_app(Preset::Type type, bool reload_tab)
{
    if (Tab *tab = wxGetApp().get_tab(type)) {
        if (reload_tab)
            tab->load_current_preset();
        else
            tab->update_tab_ui();
        tab->update_dirty();
    }
    if (Plater *plater = wxGetApp().plater())
        plater->sidebar().update_presets(type);
}

void PresetManagerDialog::on_dpi_changed(const wxRect &)
{
    Layout();
    Refresh();
}

}} // namespace Slic3r::GUI
