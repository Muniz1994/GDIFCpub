
// 1. Include the main IfcParse header
#include "../IfcFile.h"
#include "../IfcLogger.h"
#include "../FileReader.h"

#include <boost/optional/optional_io.hpp>

// 2. Include the header for the specific schema you want to use.
//    This is required for the schema-aware 'instances_by_type' template.
//    You can include Ifc2x3.h, Ifc4.h, Ifc4x3_add2.h, etc.
#include "../Ifc4.h"

// 3. Define which schema to use. This macro tells the templated
//    functions which schema's classes to instantiate.
//    This must match the header you included.
#define IfcSchema Ifc4

int main() {

    Logger::SetOutput(&std::cout, &std::cout);
    
    const std::string file_path = "C:/Users/engbr/Documents/GitHub/ifcparse/out/build/x64-Debug/Teste.ifc";


    // 4. Open the IFC file
    //    The IfcFile constructor takes the path and parses the file.
    IfcParse::IfcFile file(file_path);
  
    //if (!file.good()) {
    //    std::cout << "Unable to parse .ifc file" << std::endl;
    //    return 1;
    //}

    auto elements = file.instances_by_type<IfcSchema::IfcWall>();

    for (auto wall : *elements)
    {
        auto relDefines = wall->IsDefinedBy();

        for (auto rel : *relDefines)
        {
            auto p_def = rel->RelatingPropertyDefinition();
            
            if (p_def->as<Ifc4::IfcPropertySet>())
            {
                auto pset = p_def->as<Ifc4::IfcPropertySet>();

                std::cout << pset->GlobalId();

            }
        }
    }

    return 0;
}