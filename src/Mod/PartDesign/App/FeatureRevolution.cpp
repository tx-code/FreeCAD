/***************************************************************************
 *   Copyright (c) 2010 Juergen Riegel <FreeCAD@juergen-riegel.net>        *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/


#include "PreCompiled.h"
#ifndef _PreComp_
# include <BRepAlgoAPI_Fuse.hxx>
# include <BRepPrimAPI_MakeRevol.hxx>
# include <BRepFeat_MakeRevol.hxx>
# include <gp_Lin.hxx>
# include <Precision.hxx>
# include <TopExp_Explorer.hxx>
# include <TopoDS.hxx>
#endif

#include <Base/Axis.h>
#include <Base/Exception.h>
#include <Base/Placement.h>
#include <Base/Tools.h>

#include "FeatureRevolution.h"

using namespace PartDesign;

namespace PartDesign {

const char* Revolution::TypeEnums[]= {"Angle", "UpToLast", "UpToFirst", "UpToFace", "TwoAngles", nullptr};

PROPERTY_SOURCE(PartDesign::Revolution, PartDesign::ProfileBased)

const App::PropertyAngle::Constraints Revolution::floatAngle = { Base::toDegrees<double>(Precision::Angular()), 360.0, 1.0 };

Revolution::Revolution()
{
    addSubType = FeatureAddSub::Additive;

    ADD_PROPERTY_TYPE(Type, (0L), "Revolution", App::Prop_None, "Revolution type");
    Type.setEnums(TypeEnums);
    ADD_PROPERTY_TYPE(Base,(Base::Vector3d(0.0,0.0,0.0)),"Revolution", App::Prop_ReadOnly, "Base");
    ADD_PROPERTY_TYPE(Axis,(Base::Vector3d(0.0,1.0,0.0)),"Revolution", App::Prop_ReadOnly, "Axis");
    ADD_PROPERTY_TYPE(Angle,(360.0),"Revolution", App::Prop_None, "Angle");
    ADD_PROPERTY_TYPE(UpToFace, (nullptr), "Revolution", App::Prop_None, "Face where revolution will end");
    ADD_PROPERTY_TYPE(Angle2, (60.0), "Revolution", App::Prop_None, "Revolution length in 2nd direction");

    Angle.setConstraints(&floatAngle);
    ADD_PROPERTY_TYPE(ReferenceAxis,(nullptr),"Revolution",(App::Prop_None),"Reference axis of revolution");
}

short Revolution::mustExecute() const
{
    if (Placement.isTouched() ||
        ReferenceAxis.isTouched() ||
        Axis.isTouched() ||
        Base.isTouched() ||
        UpToFace.isTouched() ||
        Angle.isTouched() ||
        Angle2.isTouched())
        return 1;
    return ProfileBased::mustExecute();
}

App::DocumentObjectExecReturn *Revolution::execute()
{
    // Validate parameters
    // All angles are in radians unless explicitly stated
    double angleDeg = Angle.getValue();
    if (angleDeg > 360.0)
        return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP("Exception", "Angle of revolution too large"));

    double angle = Base::toRadians<double>(angleDeg);
    if (angle < Precision::Angular())
        return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP("Exception", "Angle of revolution too small"));

    double angle2 = Base::toRadians(Angle2.getValue());

    // Reverse angle if selected
    if (Reversed.getValue() && !Midplane.getValue())
        angle *= (-1.0);

    TopoDS_Shape sketchshape;
    try {
        sketchshape = getVerifiedFace();
    } catch (const Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }

    // if the Base property has a valid shape, fuse the AddShape into it
    TopoDS_Shape base;
    try {
        base = getBaseShape();
    } catch (const Base::Exception&) {
        // fall back to support (for legacy features)
        base = TopoDS_Shape();
    }

    // update Axis from ReferenceAxis
    try {
        updateAxis();
    } catch (const Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }

    // get revolve axis
    Base::Vector3d b = Base.getValue();
    gp_Pnt pnt(b.x,b.y,b.z);
    Base::Vector3d v = Axis.getValue();
    gp_Dir dir(v.x,v.y,v.z);

    try {
        if (sketchshape.IsNull())
            return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP("Exception", "Creating a face from sketch failed"));

        std::string method(Type.getValueAsString());

        // Rotate the face by half the angle to get Revolution symmetric to sketch plane
        if (Midplane.getValue()) {
            gp_Trsf mov;
            mov.SetRotation(gp_Ax1(pnt, dir), Base::toRadians<double>(Angle.getValue()) * (-1.0) / 2.0);
            TopLoc_Location loc(mov);
            sketchshape.Move(loc);
        }
        else if (method == "TwoAngles") {
            gp_Trsf mov;
            mov.SetRotation(gp_Ax1(pnt, dir), angle2 * (-1.0));
            TopLoc_Location loc(mov);
            sketchshape.Move(loc);

            angle = angle + angle2;
        }

        this->positionByPrevious();
        TopLoc_Location invObjLoc = this->getLocation().Inverted();
        pnt.Transform(invObjLoc.Transformation());
        dir.Transform(invObjLoc.Transformation());
        base.Move(invObjLoc);
        sketchshape.Move(invObjLoc);

        // Check distance between sketchshape and axis - to avoid failures and crashes
        TopExp_Explorer xp;
        xp.Init(sketchshape, TopAbs_FACE);
        for (;xp.More(); xp.Next()) {
            if (checkLineCrossesFace(gp_Lin(pnt, dir), TopoDS::Face(xp.Current())))
                return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP("Exception", "Revolve axis intersects the sketch"));
        }

        TopoDS_Shape result;
        TopoDS_Face supportface = getSupportFace();
        supportface.Move(invObjLoc);

        if (method == "UpToFace" || method == "UpToFirst" || method == "UpToLast") {
            TopoDS_Face upToFace;
            if (method == "UpToFace") {
                getFaceFromLinkSub(upToFace, UpToFace);
                upToFace.Move(invObjLoc);
            }
            else
                throw Base::RuntimeError("ProfileBased: Revolution up to first/last is not yet supported");

            getUpToFace(upToFace, base, sketchshape, method, dir);

            // TODO: Make enum
            int mode = 2;
            BRepFeat_MakeRevol RevolMaker;
            for (TopExp_Explorer xp(sketchshape, TopAbs_FACE); xp.More(); xp.Next()) {
                RevolMaker.Init(base, xp.Current(), supportface, gp_Ax1(pnt, dir), mode, Standard_True);
                RevolMaker.Perform(upToFace);

                if (!RevolMaker.IsDone())
                    throw Base::RuntimeError("ProfileBased: Up to face: Could not revolve the sketch!");

                base = RevolMaker.Shape();
                if (mode == 2)
                    mode = 1;
            }

            result = base;
        }
        else {
            // TODO: Make enum
            int mode = 2;
            BRepFeat_MakeRevol RevolMaker;
            for (TopExp_Explorer xp(sketchshape, TopAbs_FACE); xp.More(); xp.Next()) {
                RevolMaker.Init(base, xp.Current(), supportface, gp_Ax1(pnt, dir), mode, Standard_True);
                RevolMaker.Perform(angle);

                if (!RevolMaker.IsDone())
                    throw Base::RuntimeError("ProfileBased: Could not revolve the sketch!");

                base = RevolMaker.Shape();
                if (mode == 2)
                    mode = 1;
            }

            if (RevolMaker.IsDone())
                result = RevolMaker.Shape();
        }

        if (!result.IsNull()) {
            result = refineShapeIfActive(result);
            // set the additive shape property for later usage in e.g. pattern
            this->AddSubShape.setValue(result);

            if (!base.IsNull()) {
                // Let's call algorithm computing a fuse operation:
                BRepAlgoAPI_Fuse mkFuse(base, result);
                // Let's check if the fusion has been successful
                if (!mkFuse.IsDone())
                    throw Part::BooleanException(QT_TRANSLATE_NOOP("Exception", "Fusion with base feature failed"));
                result = mkFuse.Shape();
                result = refineShapeIfActive(result);
            }

            this->Shape.setValue(getSolid(result));
        }
        else
            return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP("Exception", "Could not revolve the sketch!"));

        return App::DocumentObject::StdReturn;
    }
    catch (Standard_Failure& e) {

        if (std::string(e.GetMessageString()) == "TopoDS::Face")
            return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP("Exception", "Could not create face from sketch.\n"
                "Intersecting sketch entities in a sketch are not allowed."));
        else
            return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
    catch (Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }
}

bool Revolution::suggestReversed()
{
    try {
        updateAxis();
    } catch (const Base::Exception&) {
        return false;
    }

    return ProfileBased::getReversedAngle(Base.getValue(), Axis.getValue()) < 0.0;
}

void Revolution::updateAxis()
{
    App::DocumentObject *pcReferenceAxis = ReferenceAxis.getValue();
    const std::vector<std::string> &subReferenceAxis = ReferenceAxis.getSubValues();
    Base::Vector3d base;
    Base::Vector3d dir;
    getAxis(pcReferenceAxis, subReferenceAxis, base, dir, ForbiddenAxis::NotParallelWithNormal);

    Base.setValue(base.x,base.y,base.z);
    Axis.setValue(dir.x,dir.y,dir.z);
}

}
