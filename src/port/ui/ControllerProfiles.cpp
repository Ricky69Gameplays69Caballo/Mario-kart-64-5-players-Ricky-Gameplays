#include "ControllerProfiles.h"
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <array>
#include <cctype>
#include <cstdio>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include "ship/Context.h"
#include "ship/config/Config.h"
#include "ship/config/ConsoleVariable.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/controller/controldevice/controller/Controller.h"

namespace ControllerProfiles {
namespace {
using Json = nlohmann::json;
constexpr const char* controllerKey = "gControllers";
constexpr std::array<const char*, 5> categories = {
    "ButtonMappings", "AxisDirectionMappings", "RumbleMappings", "GyroMappings", "LEDMappings"
};
struct PortUI {
    std::string selected;
    std::string status;
    char name[96]{};
    int operation = 0; // 0: save as, 1: rename
};
std::array<PortUI, 16> ports;

std::string TrimName(const char* name) {
    std::string result(name);
    const auto first = result.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return result.substr(first, result.find_last_not_of(" \t\r\n") - first + 1);
}
std::string FoldName(std::string name) {
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name;
}
Json Library() {
    auto root = Ship::Context::GetRawInstance()->GetConfig()->GetNestedJson();
    if (!root.contains("ControllerProfiles")) return Json::array();
    const auto& block = root["ControllerProfiles"];
    if (!block.is_object() || block.value("Version", 0) != 1 ||
        !block.contains("Profiles") || !block["Profiles"].is_array()) return Json::array();
    return block["Profiles"];
}
void SaveLibrary(const Json& library) {
    Ship::Context::GetRawInstance()->GetConfig()->SetBlock("ControllerProfiles", {{"Version", 1}, {"Profiles", library}});
}
int Find(const Json& library, const std::string& name) {
    for (int i = 0; i < static_cast<int>(library.size()); ++i) {
        if (library[i].is_object() && library[i].contains("Name") && library[i]["Name"].is_string() &&
            FoldName(library[i]["Name"].get<std::string>()) == FoldName(name)) return i;
    }
    return -1;
}
bool OwnsId(const std::string& id, int port) {
    const auto prefix = "P" + std::to_string(port);
    return id == prefix || id.starts_with(prefix + "-");
}
std::string RemapId(const std::string& id, int from, int to) {
    if (!OwnsId(id, from)) throw std::runtime_error("Profile contains a mapping for another port.");
    return "P" + std::to_string(to) + id.substr(1 + std::to_string(from).size());
}
void RemapReferences(Json& node, int from, int to) {
    for (auto& [key, value] : node.items()) {
        if (value.is_object()) RemapReferences(value, from, to);
        else if (value.is_string() && (key.ends_with("MappingId") || key.ends_with("MappingIds"))) {
            std::istringstream input(value.get<std::string>());
            std::string id, rewritten;
            while (std::getline(input, id, ',')) {
                if (id.empty()) continue;
                if (!rewritten.empty()) rewritten += ',';
                rewritten += RemapId(id, from, to);
            }
            value = rewritten;
        }
    }
}

// Traverse CVar values as typed leaves. LED colors must remain one color
// variable rather than becoming unrelated R/G/B variables.
void VisitValues(const Json& node, const std::string& path,
                 const std::function<void(const std::string&, const Json&)>& visit) {
    if (node.is_object() && !node.contains("Type")) {
        for (const auto& [key, value] : node.items()) VisitValues(value, path + "." + key, visit);
    } else if (!node.is_null()) visit(path, node);
}
void SetValue(const std::string& key, const Json& value) {
    auto vars = Ship::Context::GetRawInstance()->GetConsoleVariables();
    if (value.is_string()) vars->SetString(key.c_str(), value.get<std::string>().c_str());
    else if (value.is_boolean()) vars->SetInteger(key.c_str(), value.get<bool>());
    else if (value.is_number_integer()) vars->SetInteger(key.c_str(), value.get<int32_t>());
    else if (value.is_number_float()) vars->SetFloat(key.c_str(), value.get<float>());
    else if (value.is_object() && value.value("Type", "") == "RGB")
        vars->SetColor24(key.c_str(), {value.at("R").get<uint8_t>(), value.at("G").get<uint8_t>(), value.at("B").get<uint8_t>()});
    else if (value.is_object() && value.value("Type", "") == "RGBA")
        vars->SetColor(key.c_str(), {value.at("R").get<uint8_t>(), value.at("G").get<uint8_t>(),
                                   value.at("B").get<uint8_t>(), value.at("A").get<uint8_t>()});
    else throw std::runtime_error("Unsupported setting in controller profile.");
}
Json Capture(int port, const std::string& name) {
    auto context = Ship::Context::GetRawInstance();
    context->GetConsoleVariables()->Save();
    auto root = context->GetConfig()->GetNestedJson();
    auto settings = root.value("CVars", Json::object()).value(controllerKey, Json::object());
    Json profile = {{"Name", name}, {"SourcePort", port}, {"Settings", Json::object()}};
    profile["Settings"]["Port"] = settings.value("Port" + std::to_string(port + 1), Json::object());
    profile["Settings"]["Port"]["HasConfig"] = 1;
    for (const auto* category : categories) {
        profile["Settings"][category] = Json::object();
        if (!settings.contains(category) || !settings[category].is_object()) continue;
        for (const auto& [id, mapping] : settings[category].items())
            if (OwnsId(id, port)) profile["Settings"][category][id] = mapping;
    }
    return profile;
}
void Apply(const Json& profile, int port) {
    const int from = profile.at("SourcePort").get<int>();
    if (from < 0 || from >= 16 || !profile.at("Settings").is_object())
        throw std::runtime_error("Invalid controller profile.");
    Json incoming = profile.at("Settings");
    if (!incoming.contains("Port") || !incoming["Port"].is_object())
        throw std::runtime_error("Profile is missing its bindings.");
    RemapReferences(incoming["Port"], from, port);
    incoming["Port"]["HasConfig"] = 1;
    for (const auto* category : categories) {
        Json remapped = Json::object();
        // Config's flatten/unflatten represents an empty object as null.
        // Unused gyro/LED/rumble sections are therefore valid empty sections.
        if (incoming.contains(category) && !incoming[category].is_null()) {
            if (!incoming[category].is_object()) throw std::runtime_error("Invalid profile mappings.");
            for (const auto& [id, mapping] : incoming[category].items()) remapped[RemapId(id, from, port)] = mapping;
        }
        incoming[category] = remapped;
    }
    // Work only on this port's CVars. Reloading the entire CVar store would
    // invalidate pointers cached by unrelated game/UI settings.
    auto context = Ship::Context::GetRawInstance();
    auto vars = context->GetConsoleVariables();
    const Json previous = Capture(port, "");
    auto replace = [&](const Json& settings) {
        const auto portKey = std::string(controllerKey) + ".Port" + std::to_string(port + 1);
        VisitValues(previous["Settings"]["Port"], portKey, [&](const std::string& key, const Json&) { vars->ClearVariable(key.c_str()); });
        for (const auto* category : categories) {
            VisitValues(previous["Settings"][category], std::string(controllerKey) + "." + category,
                        [&](const std::string& key, const Json&) { vars->ClearVariable(key.c_str()); });
        }
        VisitValues(settings["Port"], portKey, SetValue);
        for (const auto* category : categories)
            VisitValues(settings[category], std::string(controllerKey) + "." + category, SetValue);
        vars->Save();
        auto controller = context->GetControlDeck()->GetControllerByPort(static_cast<uint8_t>(port));
        for (auto& [id, mapping] : controller->GetRumble()->GetAllRumbleMappings()) mapping->StopRumble();
        controller->ReloadAllMappingsFromConfig();
    };
    try {
        replace(incoming);
    } catch (...) {
        // Restore this port if a saved profile cannot be decoded.
        VisitValues(incoming["Port"], std::string(controllerKey) + ".Port" + std::to_string(port + 1),
                    [&](const std::string& key, const Json&) { vars->ClearVariable(key.c_str()); });
        for (const auto* category : categories)
            VisitValues(incoming[category], std::string(controllerKey) + "." + category,
                        [&](const std::string& key, const Json&) { vars->ClearVariable(key.c_str()); });
        replace(previous["Settings"]);
        throw;
    }
}
} // namespace

bool DrawForPort(uint8_t port) {
    if (port >= ports.size()) return false;
    auto& ui = ports[port];
    auto library = Library();
    int selected = Find(library, ui.selected);
    ImGui::PushID("ControllerProfiles");
    ImGui::PushID(port);
    ImGui::TextUnformatted("Controller profiles");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo("##profile", selected < 0 ? "Select a profile" : ui.selected.c_str())) {
        for (int i = 0; i < static_cast<int>(library.size()); ++i) {
            if (!library[i].is_object() || !library[i].contains("Name") || !library[i]["Name"].is_string()) continue;
            const auto name = library[i]["Name"].get<std::string>();
            if (ImGui::Selectable(name.c_str(), i == selected)) { ui.selected = name; ui.status.clear(); selected = i; }
        }
        ImGui::EndCombo();
    }
    auto execute = [&](const std::function<void()>& operation) {
        try { operation(); }
        catch (const std::exception& error) { ui.status = std::string("Could not complete: ") + error.what(); }
    };
    ImGui::BeginDisabled(selected < 0);
    if (ImGui::Button("Load")) execute([&] {
        Apply(library[selected], port);
        ui.status = "Loaded " + ui.selected + " on Port " + std::to_string(port + 1) + ".";
    });
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Save as...")) {
        ui.name[0] = '\0'; ui.operation = 0; ui.status.clear();
        ImGui::OpenPopup("Profile name");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(selected < 0);
    if (ImGui::Button("Update")) ImGui::OpenPopup("Update profile");
    ImGui::SameLine();
    if (ImGui::Button("Rename")) {
        std::snprintf(ui.name, sizeof(ui.name), "%s", ui.selected.c_str());
        ui.operation = 1; ui.status.clear(); ImGui::OpenPopup("Profile name");
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) ImGui::OpenPopup("Delete profile");
    ImGui::EndDisabled();
    ImGui::TextWrapped("Load onto any port. Profiles include bindings, sticks, rumble, gyro and LED settings.");
    if (!ui.status.empty()) ImGui::TextWrapped("%s", ui.status.c_str());
    if (ImGui::BeginPopupModal("Profile name", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(ui.operation == 0 ? "Save this port's current setup as a new profile." : "Rename the selected profile.");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("Name", ui.name, sizeof(ui.name), ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string name = TrimName(ui.name);
        const int existing = Find(library, name);
        const bool valid = !name.empty() && (existing < 0 || (ui.operation == 1 && existing == selected));
        if (!name.empty() && !valid) ImGui::TextUnformatted("That name already exists. Use Update to replace its setup.");
        ImGui::BeginDisabled(!valid);
        const bool clicked = ImGui::Button(ui.operation == 0 ? "Save profile" : "Rename profile");
        if (valid && (clicked || enter)) execute([&] {
            if (ui.operation == 0) library.push_back(Capture(port, name));
            else library[selected]["Name"] = name;
            SaveLibrary(library); ui.selected = name; ui.status = "Saved profile: " + name;
            ImGui::CloseCurrentPopup();
        });
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Update profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Replace '%s' with Port %d's current setup?", ui.selected.c_str(), port + 1);
        if (ImGui::Button("Update profile") && selected >= 0) execute([&] {
            library[selected] = Capture(port, ui.selected); SaveLibrary(library);
            ui.status = "Updated profile: " + ui.selected; ImGui::CloseCurrentPopup();
        });
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Delete profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete '%s'? The port's current bindings will stay active.", ui.selected.c_str());
        if (ImGui::Button("Delete profile") && selected >= 0) execute([&] {
            library.erase(library.begin() + selected); SaveLibrary(library);
            ui.selected.clear(); ui.status = "Profile deleted."; ImGui::CloseCurrentPopup();
        });
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    const bool popup = ImGui::IsPopupOpen("Profile name") || ImGui::IsPopupOpen("Update profile") || ImGui::IsPopupOpen("Delete profile");
    ImGui::Separator();
    ImGui::PopID(); ImGui::PopID();
    return popup;
}
} // namespace ControllerProfiles
