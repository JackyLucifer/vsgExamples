/* <editor-fold desc="MIT License">

Copyright(c) 2019 Robert Osfield

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

</editor-fold> */

#include <vsg/io/Options.h>
#include "GlobeTrackball.h"

#include <iostream>

using namespace vsg;

GlobeTrackball::GlobeTrackball(ref_ptr<Camera> camera, ref_ptr<EllipsoidModel> ellipsoidModel) :
    _camera(camera),
    _ellipsoidModel(ellipsoidModel)
{
    _lookAt = dynamic_cast<LookAt*>(_camera->getViewMatrix());

    if (!_lookAt)
    {
        // TODO: need to work out how to map the original ViewMatrix to a LookAt and back, for now just fallback to assigning our own LookAt
        _lookAt = new LookAt;
    }

    clampToGlobe();

    _homeLookAt = new LookAt(_lookAt->eye, _lookAt->center, _lookAt->up);
}

void GlobeTrackball::clampToGlobe()
{
//    std::cout<<"GlobeTrackball::clampToGlobe()"<<std::endl;

    if (!_ellipsoidModel) return;

    // get the location of the current lookAt center
    auto location_center = _ellipsoidModel->convertECEFToLatLongAltitude(_lookAt->center);
    auto location_eye = _ellipsoidModel->convertECEFToLatLongAltitude(_lookAt->eye);

    double ratio = location_eye.z / (location_eye.z - location_center.z);
    auto location = _ellipsoidModel->convertECEFToLatLongAltitude(_lookAt->center * ratio + _lookAt->eye * (1.0 - ratio));

    // clamp to the globe
    location.z = 0.0;

    // compute clamped position back in ECEF
    auto ecef = _ellipsoidModel->convertLatLongAltitudeToECEF(location);

    // apply the new clamped position to the LookAt.
    _lookAt->center = ecef;

    double minimum_altitude = 1.0;
    if (location_eye.z < minimum_altitude)
    {
        location_eye.z = minimum_altitude;
        _lookAt->eye = _ellipsoidModel->convertLatLongAltitudeToECEF(location_eye);
    }
}


bool GlobeTrackball::withinRenderArea(int32_t x, int32_t y) const
{
    auto renderArea = _camera->getRenderArea();

    return (x >= renderArea.offset.x && x < static_cast<int32_t>(renderArea.offset.x + renderArea.extent.width)) &&
           (y >= renderArea.offset.y && y < static_cast<int32_t>(renderArea.offset.y + renderArea.extent.height));
}

/// compute non dimensional window coordinate (-1,1) from event coords
dvec2 GlobeTrackball::ndc(PointerEvent& event)
{
    auto renderArea = _camera->getRenderArea();

    double aspectRatio = static_cast<double>(renderArea.extent.width) / static_cast<double>(renderArea.extent.height);
    dvec2 v(
        (renderArea.extent.width > 0) ? (static_cast<double>(event.x - renderArea.offset.x) / static_cast<double>(renderArea.extent.width) * 2.0 - 1.0) * aspectRatio : 0.0,
        (renderArea.extent.height > 0) ? static_cast<double>(event.y - renderArea.offset.y) / static_cast<double>(renderArea.extent.height) * 2.0 - 1.0 : 0.0);
    return v;
}

/// compute trackball coordinate from event coords
dvec3 GlobeTrackball::tbc(PointerEvent& event)
{
    dvec2 v = ndc(event);

    double l = length(v);
    if (l < 1.0f)
    {
        double h = 0.5 + cos(l * PI) * 0.5;
        return dvec3(v.x, -v.y, h);
    }
    else
    {
        return dvec3(v.x, -v.y, 0.0);
    }
}

void GlobeTrackball::apply(KeyPressEvent& keyPress)
{
    if (keyPress.handled || !_lastPointerEventWithinRenderArea) return;

    if (keyPress.keyBase == _homeKey)
    {
        keyPress.handled = true;

        LookAt* lookAt = dynamic_cast<LookAt*>(_camera->getViewMatrix());
        if (lookAt && _homeLookAt)
        {
            lookAt->eye = _homeLookAt->eye;
            lookAt->center = _homeLookAt->center;
            lookAt->up = _homeLookAt->up;
        }
    }
}

void GlobeTrackball::apply(ButtonPressEvent& buttonPress)
{
    prev_ndc = ndc(buttonPress);
    prev_tbc = tbc(buttonPress);

    if (buttonPress.handled) return;

    _hasFocus = withinRenderArea(buttonPress.x, buttonPress.y);
    _lastPointerEventWithinRenderArea = _hasFocus;

    if (buttonPress.mask & BUTTON_MASK_1) _updateMode = ROTATE;
    else if (buttonPress.mask & BUTTON_MASK_2) _updateMode = PAN;
    else if (buttonPress.mask & BUTTON_MASK_3) _updateMode = ZOOM;
    else _updateMode = INACTIVE;

    if (_hasFocus) buttonPress.handled = true;

    _zoomPreviousRatio = 0.0;
    _pan.set(0.0, 0.0);
    _rotateAngle = 0.0;
}

void GlobeTrackball::apply(ButtonReleaseEvent& buttonRelease)
{
    prev_ndc = ndc(buttonRelease);
    prev_tbc = tbc(buttonRelease);

    _lastPointerEventWithinRenderArea = withinRenderArea(buttonRelease.x, buttonRelease.y);
    _hasFocus = false;
    _updateMode = INACTIVE;
    _zoomPreviousRatio = 0.0;
    _pan.set(0.0, 0.0);
    _rotateAngle = 0.0;
}

void GlobeTrackball::apply(MoveEvent& moveEvent)
{
    _lastPointerEventWithinRenderArea = withinRenderArea(moveEvent.x, moveEvent.y);

    if (moveEvent.handled || !_hasFocus) return;

    dvec2 new_ndc = ndc(moveEvent);
    dvec3 new_tbc = tbc(moveEvent);

    if (moveEvent.mask & BUTTON_MASK_1)
    {
        moveEvent.handled = true;

        dvec3 xp = cross(normalize(new_tbc), normalize(prev_tbc));
        double xp_len = length(xp);
        if (xp_len > 0.0)
        {
#if 1
            _rotateAngle = asin(xp_len);
            _rotateAxis = xp / xp_len;
#else
            dvec3 axis = xp / xp_len;
            double angle = asin(xp_len);
            rotate(angle, axis);
#endif
        }
        else
        {
            _rotateAngle = 0.0;
        }
    }
    else if (moveEvent.mask & BUTTON_MASK_2)
    {
        moveEvent.handled = true;

        dvec2 delta = new_ndc - prev_ndc;

#if 1
        _pan = delta;
#else
        pan(delta);
#endif
    }
    else if (moveEvent.mask & BUTTON_MASK_3)
    {
        moveEvent.handled = true;

        dvec2 delta = new_ndc - prev_ndc;

        if (delta.y != 0.0)
        {
    #if 0
            zoom(delta.y);
    #else
            _zoomPreviousRatio = 2.0 * delta.y;
    #endif
        }
    }

    prev_ndc = new_ndc;
    prev_tbc = new_tbc;
}

void GlobeTrackball::apply(ScrollWheelEvent& scrollWheel)
{
    if (scrollWheel.handled) return;

    scrollWheel.handled = true;

    zoom(scrollWheel.delta.y * 0.1);
}

void GlobeTrackball::apply(FrameEvent& frame)
{
    //    std::cout<<"Frame "<<frame.frameStamp->frameCount<<std::endl;
    //std::cout<<"Zoom active "<<_zoomActive<<std::endl;

    if (!first_frame)
    {
        switch(_updateMode)
        {
            case(ZOOM):
                if (_zoomPreviousRatio != 0.0)
                {
                    double dt = std::chrono::duration<double, std::chrono::seconds::period>(frame.time-prev_time).count();
                    zoom(_zoomPreviousRatio * dt * 60.0);
                }
                break;
            case(PAN):
                if (_pan != dvec2(0.0, 0.0))
                {
                    double dt = std::chrono::duration<double, std::chrono::seconds::period>(frame.time-prev_time).count();
                    pan(_pan  * (dt * 60.0));
                }
                break;
            case(ROTATE):
                if (_rotateAngle != 0.0)
                {
                    double dt = std::chrono::duration<double, std::chrono::seconds::period>(frame.time-prev_time).count();
                    rotate( _rotateAngle * (dt * 60.0), _rotateAxis);
                }
                break;
            default:
                break;
        }

    }
    else first_frame = false;

    prev_time = frame.time;
}

void GlobeTrackball::rotate(double angle, const dvec3& axis)
{
    dmat4 rotation = vsg::rotate(angle, axis);
    dmat4 lv = lookAt(_lookAt->eye, _lookAt->center, _lookAt->up);
    dvec3 centerEyeSpace = (lv * _lookAt->center);

    dmat4 matrix = inverse(lv) * translate(centerEyeSpace) * rotation * translate(-centerEyeSpace) * lv;

    _lookAt->up = normalize(matrix * (_lookAt->eye + _lookAt->up) - matrix * _lookAt->eye);
    _lookAt->center = matrix * _lookAt->center;
    _lookAt->eye = matrix * _lookAt->eye;

    clampToGlobe();
}

void GlobeTrackball::zoom(double ratio)
{
    dvec3 lookVector = _lookAt->center - _lookAt->eye;
    _lookAt->eye = _lookAt->eye + lookVector * ratio;

    clampToGlobe();
}

void GlobeTrackball::pan(const dvec2& delta)
{
    dvec3 lookVector = _lookAt->center - _lookAt->eye;
    dvec3 lookNormal = normalize(lookVector);
    dvec3 upNormal = _lookAt->up;
    dvec3 sideNormal = cross(lookNormal, upNormal);

    double distance = length(lookVector);
    distance *= 0.3; // TODO use Camera project matrix to guide how much to scale

#if 1
    if (_ellipsoidModel)
    {

        dvec3 globeNormal = normalize(_lookAt->center);

        double scale = distance;
        dvec3 m = upNormal * (-scale*delta.y) + sideNormal * (scale * delta.x);
        dvec3 v = m + lookNormal * dot(m, globeNormal);
        double angle = length(v)/_ellipsoidModel->radiusEquator();

        if (angle != 0.0)
        {
            dvec3 n = normalize(_lookAt->center + v);
            dvec3 axis = normalize(cross(globeNormal, n));

            dmat4 matrix = vsg::rotate(-angle , axis);

            _lookAt->up = normalize(matrix * (_lookAt->eye + _lookAt->up) - matrix * _lookAt->eye);
            _lookAt->center = matrix * _lookAt->center;
            _lookAt->eye = matrix * _lookAt->eye;

            clampToGlobe();
        }


    }
    else
#endif
    {
        dvec3 translation = sideNormal * (-delta.x * distance) + upNormal * (delta.y * distance);

        _lookAt->eye = _lookAt->eye + translation;
        _lookAt->center = _lookAt->center + translation;
    }

}
