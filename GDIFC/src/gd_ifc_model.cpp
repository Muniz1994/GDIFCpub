#include "gd_ifc_model.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <IfcSchema.h>
#include <IfcParse.h>  // for operator<< serialisation

#include <fstream>
#include <sstream>

using namespace godot;

// ── Lifecycle ─────────────────────────────────────────────────────────────

void IFCModel::init(std::shared_ptr<IfcParse::IfcFile> file) {
    ifc_file_ = std::move(file);
}

// ── Header helpers ─────────────────────────────────────────────────────────

godot::String IFCModel::get_schema_name() const {
    if (!ifc_file_) return godot::String();
    const auto* fs = ifc_file_->header().file_schema();
    if (!fs) return godot::String();
    const auto& ids = fs->schema_identifiers();
    if (ids.empty()) return godot::String();
    return godot::String(ids.front().c_str());
}

godot::PackedStringArray IFCModel::get_file_description() const {
    godot::PackedStringArray result;
    if (!ifc_file_) return result;
    const auto* fd = ifc_file_->header().file_description();
    if (!fd) return result;
    for (const auto& s : fd->description()) {
        result.push_back(godot::String(s.c_str()));
    }
    return result;
}

godot::String IFCModel::get_file_name() const {
    if (!ifc_file_) return godot::String();
    const auto* fn = ifc_file_->header().file_name();
    if (!fn) return godot::String();
    return godot::String(fn->name().c_str());
}

godot::String IFCModel::get_timestamp() const {
    if (!ifc_file_) return godot::String();
    const auto* fn = ifc_file_->header().file_name();
    if (!fn) return godot::String();
    return godot::String(fn->time_stamp().c_str());
}

godot::PackedStringArray IFCModel::get_author() const {
    godot::PackedStringArray result;
    if (!ifc_file_) return result;
    const auto* fn = ifc_file_->header().file_name();
    if (!fn) return result;
    for (const auto& s : fn->author()) {
        result.push_back(godot::String(s.c_str()));
    }
    return result;
}

godot::PackedStringArray IFCModel::get_organization() const {
    godot::PackedStringArray result;
    if (!ifc_file_) return result;
    const auto* fn = ifc_file_->header().file_name();
    if (!fn) return result;
    for (const auto& s : fn->organization()) {
        result.push_back(godot::String(s.c_str()));
    }
    return result;
}

godot::String IFCModel::get_preprocessor() const {
    if (!ifc_file_) return godot::String();
    const auto* fn = ifc_file_->header().file_name();
    if (!fn) return godot::String();
    return godot::String(fn->preprocessor_version().c_str());
}

godot::String IFCModel::get_originating_system() const {
    if (!ifc_file_) return godot::String();
    const auto* fn = ifc_file_->header().file_name();
    if (!fn) return godot::String();
    return godot::String(fn->originating_system().c_str());
}

godot::String IFCModel::get_authorization() const {
    if (!ifc_file_) return godot::String();
    const auto* fn = ifc_file_->header().file_name();
    if (!fn) return godot::String();
    return godot::String(fn->authorization().c_str());
}

// ── Entity lookup ─────────────────────────────────────────────────────────

Ref<GDIFCEntityBase> IFCModel::get_by_id(int64_t express_id) {
    if (!ifc_file_) return {};
    try {
        auto* inst = ifc_file_->instance_by_id(static_cast<int>(express_id));
        if (!inst) return {};
        return GDIFCEntityBase::wrap(inst, ifc_file_);
    } catch (...) {
        return {};
    }
}

Ref<GDIFCEntityBase> IFCModel::get_by_global_id(godot::String global_id) {
    if (!ifc_file_) return {};
    try {
        auto* inst = ifc_file_->instance_by_guid(
            std::string(global_id.utf8().get_data()));
        if (!inst) return {};
        return GDIFCEntityBase::wrap(inst, ifc_file_);
    } catch (...) {
        return {};
    }
}

godot::Array IFCModel::instances_by_type(godot::String ifc_class) {
    godot::Array result;
    if (!ifc_file_) return result;
    try {
        std::string name = ifc_class.utf8().get_data();
        auto instances = ifc_file_->instances_by_type(name);
        if (instances) {
            for (auto* inst : *instances) {
                result.push_back(GDIFCEntityBase::wrap(inst, ifc_file_));
            }
        }
    } catch (...) {}
    return result;
}

godot::Array IFCModel::instances_by_type_exact(godot::String ifc_class) {
    godot::Array result;
    if (!ifc_file_) return result;
    try {
        std::string name = ifc_class.utf8().get_data();
        auto instances = ifc_file_->instances_by_type_excl_subtypes(name);
        if (instances) {
            for (auto* inst : *instances) {
                result.push_back(GDIFCEntityBase::wrap(inst, ifc_file_));
            }
        }
    } catch (...) {}
    return result;
}

Ref<GDIFCEntityBase> IFCModel::get_project() {
    if (!ifc_file_) return {};
    try {
        auto instances = ifc_file_->instances_by_type("IfcProject");
        if (instances && instances->size() > 0) {
            return GDIFCEntityBase::wrap((*instances)[0], ifc_file_);
        }
        // Fallback: try IfcContext which is the supertype in newer schemas
        instances = ifc_file_->instances_by_type("IfcContext");
        if (instances && instances->size() > 0) {
            return GDIFCEntityBase::wrap((*instances)[0], ifc_file_);
        }
    } catch (...) {}
    return {};
}

// ── Entity creation ───────────────────────────────────────────────────────

Ref<GDIFCEntityBase> IFCModel::create(godot::String ifc_class) {
    if (!ifc_file_) return {};
    try {
        std::string name = ifc_class.utf8().get_data();
        const auto* schema = ifc_file_->schema();
        if (!schema) return {};
        const auto* decl = schema->declaration_by_name(name);
        if (!decl) {
            UtilityFunctions::push_error(
                godot::String("IFCModel::create: unknown IFC class: ") + ifc_class);
            return {};
        }
        auto* inst = ifc_file_->create(decl);
        if (!inst) return {};
        return GDIFCEntityBase::wrap(inst, ifc_file_);
    } catch (const std::exception& e) {
        UtilityFunctions::push_error(
            godot::String("IFCModel::create failed: ") + godot::String(e.what()));
        return {};
    }
}

// ── Serialisation ─────────────────────────────────────────────────────────

godot::Error IFCModel::save(godot::String path) {
    if (!ifc_file_) return FAILED;
    std::string std_path = path.utf8().get_data();
    try {
        std::ofstream out(std_path);
        if (!out.is_open()) {
            UtilityFunctions::push_error(
                godot::String("IFCModel::save: cannot open file: ") + path);
            return ERR_FILE_CANT_WRITE;
        }
        out << *ifc_file_;
        out.flush();
        if (!out) {
            return ERR_FILE_CANT_WRITE;
        }
        return OK;
    } catch (const std::exception& e) {
        UtilityFunctions::push_error(
            godot::String("IFCModel::save failed: ") + godot::String(e.what()));
        return FAILED;
    }
}

godot::String IFCModel::to_step_string() {
    if (!ifc_file_) return godot::String();
    try {
        std::ostringstream oss;
        oss << *ifc_file_;
        return godot::String(oss.str().c_str());
    } catch (...) {
        return godot::String();
    }
}

// ── Bind methods ──────────────────────────────────────────────────────────

void IFCModel::_bind_methods() {
    // Header
    ClassDB::bind_method(D_METHOD("get_schema_name"),       &IFCModel::get_schema_name);
    ClassDB::bind_method(D_METHOD("get_file_description"),  &IFCModel::get_file_description);
    ClassDB::bind_method(D_METHOD("get_file_name"),         &IFCModel::get_file_name);
    ClassDB::bind_method(D_METHOD("get_timestamp"),         &IFCModel::get_timestamp);
    ClassDB::bind_method(D_METHOD("get_author"),            &IFCModel::get_author);
    ClassDB::bind_method(D_METHOD("get_organization"),      &IFCModel::get_organization);
    ClassDB::bind_method(D_METHOD("get_preprocessor"),      &IFCModel::get_preprocessor);
    ClassDB::bind_method(D_METHOD("get_originating_system"),&IFCModel::get_originating_system);
    ClassDB::bind_method(D_METHOD("get_authorization"),     &IFCModel::get_authorization);

    // Lookup
    ClassDB::bind_method(D_METHOD("get_by_id", "express_id"),      &IFCModel::get_by_id);
    ClassDB::bind_method(D_METHOD("get_by_global_id", "global_id"),&IFCModel::get_by_global_id);
    ClassDB::bind_method(D_METHOD("instances_by_type", "ifc_class"),      &IFCModel::instances_by_type);
    ClassDB::bind_method(D_METHOD("instances_by_type_exact", "ifc_class"),&IFCModel::instances_by_type_exact);
    ClassDB::bind_method(D_METHOD("get_project"),           &IFCModel::get_project);

    // Creation
    ClassDB::bind_method(D_METHOD("create", "ifc_class"),   &IFCModel::create);

    // Serialisation
    ClassDB::bind_method(D_METHOD("save", "path"),          &IFCModel::save);
    ClassDB::bind_method(D_METHOD("to_step_string"),        &IFCModel::to_step_string);
}
