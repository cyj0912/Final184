//
// Copyright (c) 2008-2018 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//



#include "Rect.h"

#include <cstdio>



namespace tc
{

const Rect Rect::FULL(-1.0f, -1.0f, 1.0f, 1.0f);
const Rect Rect::POSITIVE(0.0f, 0.0f, 1.0f, 1.0f);
const Rect Rect::ZERO(0.0f, 0.0f, 0.0f, 0.0f);

const IntRect IntRect::ZERO(0, 0, 0, 0);

void IntRect::Clip(const IntRect& rect)
{
    if (rect.left_ > left_)
        left_ = rect.left_;
    if (rect.right_ < right_)
        right_ = rect.right_;
    if (rect.top_ > top_)
        top_ = rect.top_;
    if (rect.bottom_ < bottom_)
        bottom_ = rect.bottom_;

    if (left_ >= right_ || top_ >= bottom_)
        *this = IntRect();
}

void IntRect::Merge(const IntRect& rect)
{
    if (Width() <= 0 || Height() <= 0)
    {
        *this = rect;
    }
    else if (rect.Width() > 0 && rect.Height() > 0)
    {
        if (rect.left_ < left_)
            left_ = rect.left_;
        if (rect.top_ < top_)
            top_ = rect.top_;
        if (rect.right_ > right_)
            right_ = rect.right_;
        if (rect.bottom_ > bottom_)
            bottom_ = rect.bottom_;
    }
}

std::string Rect::ToString() const
{
    char tempBuffer[CONVERSION_BUFFER_LENGTH];
    sprintf(tempBuffer, "%g %g %g %g", min_.x, min_.y, max_.x, max_.y);
    return std::string(tempBuffer);
}

std::string IntRect::ToString() const
{
    char tempBuffer[CONVERSION_BUFFER_LENGTH];
    sprintf(tempBuffer, "%d %d %d %d", left_, top_, right_, bottom_);
    return std::string(tempBuffer);
}

void Rect::Clip(const Rect& rect)
{
    if (rect.min_.x > min_.x)
        min_.x = rect.min_.x;
    if (rect.max_.x < max_.x)
        max_.x = rect.max_.x;
    if (rect.min_.y > min_.y)
        min_.y = rect.min_.y;
    if (rect.max_.y < max_.y)
        max_.y = rect.max_.y;

    if (min_.x > max_.x || min_.y > max_.y)
    {
        min_ = Vector2(M_INFINITY, M_INFINITY);
        max_ = Vector2(-M_INFINITY, -M_INFINITY);
    }
}

}
