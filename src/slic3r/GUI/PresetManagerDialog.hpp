#pragma once

#include "GUI_Utils.hpp"
#include "libslic3r/Preset.hpp"

#include <string>
#include <vector>

class wxNotebook;
class wxDataViewListCtrl;
class wxDataViewEvent;
class wxSearchCtrl;
class wxCheckBox;
class wxStaticText;
class wxButton;

namespace Slic3r { namespace GUI {

// ImageMap: one window to hide/show presets (stock or user) and to save edits into stock presets.
class PresetManagerDialog : public DPIDialog
{
public:
    explicit PresetManagerDialog(wxWindow *parent);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct Page
    {
        Preset::Type             type;
        wxDataViewListCtrl      *list { nullptr };
        wxSearchCtrl            *search { nullptr };
        wxCheckBox              *stock_only { nullptr };
        wxStaticText            *selected_label { nullptr };
        wxButton                *save_edits { nullptr };
        wxButton                *reset_stock { nullptr };
        std::vector<std::string> row_names;
    };

    void build_page(wxNotebook *notebook, Page &page, const wxString &title);
    void fill_page(Page &page);
    void update_buttons(Page &page);
    void on_toggle(Page &page, wxDataViewEvent &evt);
    void set_hidden(Page &page, const std::string &name, bool hidden);
    void set_all(Page &page, bool hidden);
    void save_edits(Page &page);
    void reset_selected(Page &page);
    void refresh_app(Preset::Type type, bool reload_tab);

    std::vector<Page> m_pages;
    bool              m_filling { false };
};

}} // namespace Slic3r::GUI

