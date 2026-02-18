//
// Created by engbr on 18/02/2026.
//

#ifndef GODOTEXTENSION_GD_IFC_SETTINGS_H
#define GODOTEXTENSION_GD_IFC_SETTINGS_H

#include <godot_cpp/classes/resource.hpp>
#include <cstdint>


namespace godot {
    class GDIFCLoaderSettings: public Resource {
    GDCLASS(GDIFCLoaderSettings, Resource)
protected:
        static void _bind_methods();
private:
    // Private members with default values (using list initialization)
    bool m_coordinateToOrigin { false };
    uint16_t m_circleSegments { 12 };
    uint32_t m_tapeSize { 67108864 };
    uint32_t m_memoryLimit { 2147483648 };
    uint16_t m_lineWriterBuffer { 10000 };
    double m_tolerancePlaneIntersection { 1.0E-01 };
    double m_tolerancePlaneDeviation { 3.0E-04 };
    double m_toleranceBackDeviationDistance { 3.0E-04 };
    double m_toleranceInsideOutsidePerimeter { 1.0E-10 };
    double m_toleranceScalarEquality { 1.0E-04 };
    uint16_t m_planeRefitIterations { 10 };
    uint16_t m_booleanUnionThreshold { 150 };

public:
    // Default Constructor (Uses the values defined above)
    GDIFCLoaderSettings() = default;

    // --- Getters (Accessors) ---
    bool getCoordinateToOrigin() const { return m_coordinateToOrigin; }
    uint16_t getCircleSegments() const { return m_circleSegments; }
    uint32_t getTapeSize() const { return m_tapeSize; }
    uint32_t getMemoryLimit() const { return m_memoryLimit; }
    uint16_t getLineWriterBuffer() const { return m_lineWriterBuffer; }
    double getTolerancePlaneIntersection() const { return m_tolerancePlaneIntersection; }
    double getTolerancePlaneDeviation() const { return m_tolerancePlaneDeviation; }
    double getToleranceBackDeviationDistance() const { return m_toleranceBackDeviationDistance; }
    double getToleranceInsideOutsidePerimeter() const { return m_toleranceInsideOutsidePerimeter; }
    double getToleranceScalarEquality() const { return m_toleranceScalarEquality; }
    uint16_t getPlaneRefitIterations() const { return m_planeRefitIterations; }
    uint16_t getBooleanUnionThreshold() const { return m_booleanUnionThreshold; }

    // --- Setters (Mutators) ---
    // These return a reference to *this to allow chaining (Fluent Interface)

    void setCoordinateToOrigin(bool val) {
        m_coordinateToOrigin = val;
    }

    void setCircleSegments(uint16_t val) {
        m_circleSegments = val;
    }

    void setTapeSize(uint32_t val) {
        m_tapeSize = val;
    }

    void setMemoryLimit(uint32_t val) {
        m_memoryLimit = val;
    }

    void setLineWriterBuffer(uint16_t val) {
        m_lineWriterBuffer = val;
    }

    void setTolerancePlaneIntersection(double val) {
        m_tolerancePlaneIntersection = val;
    }

    void setTolerancePlaneDeviation(double val) {
        m_tolerancePlaneDeviation = val;
    }

    void setToleranceBackDeviationDistance(double val) {
        m_toleranceBackDeviationDistance = val;
    }

    void setToleranceInsideOutsidePerimeter(double val) {
        m_toleranceInsideOutsidePerimeter = val;
    }

    void setToleranceScalarEquality(double val) {
        m_toleranceScalarEquality = val;
    }

    void setPlaneRefitIterations(uint16_t val) {
        m_planeRefitIterations = val;
    }

    void setBooleanUnionThreshold(uint16_t val) {
        m_booleanUnionThreshold = val;
    }
};

}



#endif //GODOTEXTENSION_GD_IFC_SETTINGS_H