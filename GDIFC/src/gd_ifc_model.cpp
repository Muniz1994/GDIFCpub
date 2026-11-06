#include "gd_ifc_model.h"
#include "gd_ifc_units.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <IfcSchema.h>
#include <IfcParse.h>  // for operator<< serialisation

#include <cstring>
#include <sstream>

using namespace godot;

// ── Lifecycle ─────────────────────────────────────────────────────────────

void IFCModel::init(std::shared_ptr<IfcParse::IfcFile> file) {
    ifc_file_ = std::move(file);
}

// ── Header helpers ─────────────────────────────────────────────────────────

namespace {

// IfcParse reads header attributes on demand and throws for one it can't convert, such as $.
// Exporters often write $ for optional FILE_NAME fields (originating_system, authorization, ...),
// so an unreadable field comes back empty instead of escaping into Godot.
template <typename Read>
godot::String header_string(const IfcParse::IfcFile* file, Read read) {
    if (!file) return godot::String();
    try {
        return godot::String::utf8(read(file->header()).c_str());
    } catch (...) {
        return godot::String();
    }
}

template <typename Read>
godot::PackedStringArray header_strings(const IfcParse::IfcFile* file, Read read) {
    godot::PackedStringArray result;
    if (!file) return result;
    try {
        for (const auto& s : read(file->header())) {
            result.push_back(godot::String::utf8(s.c_str()));
        }
    } catch (...) {
        result.clear();
    }
    return result;
}

using Header = IfcParse::IfcSpfHeader;
using Strings = std::vector<std::string>;

} // namespace

godot::String IFCModel::get_schema_name() const {
    return header_string(ifc_file_.get(), [](const Header& h) {
        const auto* fs = h.file_schema();
        if (!fs) return std::string();
        const Strings ids = fs->schema_identifiers();
        return ids.empty() ? std::string() : ids.front();
    });
}

godot::PackedStringArray IFCModel::get_file_description() const {
    return header_strings(ifc_file_.get(), [](const Header& h) {
        const auto* fd = h.file_description();
        return fd ? fd->description() : Strings();
    });
}

godot::String IFCModel::get_file_name() const {
    return header_string(ifc_file_.get(), [](const Header& h) {
        const auto* fn = h.file_name();
        return fn ? fn->name() : std::string();
    });
}

godot::String IFCModel::get_timestamp() const {
    return header_string(ifc_file_.get(), [](const Header& h) {
        const auto* fn = h.file_name();
        return fn ? fn->time_stamp() : std::string();
    });
}

godot::PackedStringArray IFCModel::get_author() const {
    return header_strings(ifc_file_.get(), [](const Header& h) {
        const auto* fn = h.file_name();
        return fn ? fn->author() : Strings();
    });
}

godot::PackedStringArray IFCModel::get_organization() const {
    return header_strings(ifc_file_.get(), [](const Header& h) {
        const auto* fn = h.file_name();
        return fn ? fn->organization() : Strings();
    });
}

godot::String IFCModel::get_preprocessor() const {
    return header_string(ifc_file_.get(), [](const Header& h) {
        const auto* fn = h.file_name();
        return fn ? fn->preprocessor_version() : std::string();
    });
}

godot::String IFCModel::get_originating_system() const {
    return header_string(ifc_file_.get(), [](const Header& h) {
        const auto* fn = h.file_name();
        return fn ? fn->originating_system() : std::string();
    });
}

godot::String IFCModel::get_authorization() const {
    return header_string(ifc_file_.get(), [](const Header& h) {
        const auto* fn = h.file_name();
        return fn ? fn->authorization() : std::string();
    });
}

godot::Dictionary IFCModel::get_units() const {
    if (!ifc_file_) return {};
    return IfcUnitTable(*ifc_file_).to_dictionary();
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

godot::Dictionary IFCModel::get_site_location() const {
    godot::Dictionary result;
    if (!ifc_file_) return result;
    try {
        auto sites = ifc_file_->instances_by_type("IfcSite");
        if (!sites || sites->size() == 0) return result;
        const IfcUtil::IfcBaseClass* site = (*sites)[0];
        const IfcParse::entity* decl = site->declaration().as_entity();
        if (!decl) return result;

        // IfcCompoundPlaneAngleMeasure: degrees, minutes, seconds and millionths of a second, all
        // with the same sign
        auto angle = [&](const char* name, const char* key) {
            const ptrdiff_t index = decl->attribute_index(name);
            if (index < 0) return;
            const AttributeValue value = site->get_attribute_value(static_cast<size_t>(index));
            if (value.type() != IfcUtil::Argument_AGGREGATE_OF_INT) return;
            const std::vector<int> parts = value;
            static const double DIVISORS[] = {1.0, 60.0, 3600.0, 3600.0e6};
            double degrees = 0.0;
            for (size_t i = 0; i < parts.size() && i < 4; ++i) degrees += parts[i] / DIVISORS[i];
            result[key] = degrees;
        };
        angle("RefLatitude", "latitude");
        angle("RefLongitude", "longitude");

        const ptrdiff_t elevation = decl->attribute_index("RefElevation");
        if (elevation >= 0) {
            const AttributeValue value = site->get_attribute_value(static_cast<size_t>(elevation));
            if (value.type() == IfcUtil::Argument_DOUBLE || value.type() == IfcUtil::Argument_INT) {
                result["elevation"] = gd_attr_double(value);
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
    try {
        std::ostringstream oss;
        oss << *ifc_file_;
        const std::string data = oss.str();

        // FileAccess resolves res:// and user:// paths and works on every export platform
        Ref<FileAccess> fa = FileAccess::open(path, FileAccess::WRITE);
        if (fa.is_null()) {
            UtilityFunctions::push_error(
                godot::String("IFCModel::save: cannot open file: ") + path);
            return ERR_FILE_CANT_WRITE;
        }
        PackedByteArray bytes;
        bytes.resize(static_cast<int64_t>(data.size()));
        memcpy(bytes.ptrw(), data.data(), data.size());
        fa->store_buffer(bytes);
        return fa->get_error() == OK ? OK : ERR_FILE_CANT_WRITE;
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
        return godot::String::utf8(oss.str().c_str());
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
    ClassDB::bind_method(D_METHOD("get_units"),             &IFCModel::get_units);
    ClassDB::bind_method(D_METHOD("get_site_location"),     &IFCModel::get_site_location);

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
