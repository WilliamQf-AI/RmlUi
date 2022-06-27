/*
 * This source file is part of RmlUi, the HTML/CSS Interface Middleware
 *
 * For the latest information, see http://github.com/mikke89/RmlUi
 *
 * Copyright (c) 2008-2010 CodePoint Ltd, Shift Technology Ltd
 * Copyright (c) 2019 The RmlUi Team, and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "../../Include/RmlUi/Core/RenderState.h"
#include "../../Include/RmlUi/Core/Box.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/Geometry.h"
#include "../../Include/RmlUi/Core/GeometryUtilities.h"
#include "../../Include/RmlUi/Core/RenderInterface.h"
#include "TransformState.h"

namespace Rml {

RenderState::RenderState(RenderInterface* render_interface) : render_interface(render_interface), stack(1) {}

void RenderState::BeginRender()
{
	RMLUI_ASSERTMSG(stack.size() == 1, "Unbalanced render state push/pop detected.");

	render_interface->EnableScissorRegion(false);
	supports_stencil = render_interface->EnableClipMask(false);
	render_interface->SetTransform(nullptr);

	stack.back() = State{};
}

void RenderState::Reset()
{
	Set(State{});
}

void RenderState::DisableScissorRegion()
{
	State& state = stack.back();
	const bool scissor_enabled = (state.scissor_dimensions.x >= 0);
	if (scissor_enabled)
	{
		state.scissor_dimensions = {-1, -1};
		render_interface->EnableScissorRegion(false);
	}
}

void RenderState::EnableScissorRegion(Vector2i origin, Vector2i dimensions)
{
	RMLUI_ASSERT(dimensions.x >= 0 && dimensions.y >= 0);
	State& state = stack.back();

	const bool scissor_enabled = (state.scissor_dimensions.x >= 0);
	if (!scissor_enabled)
		render_interface->EnableScissorRegion(true);

	if (!scissor_enabled || state.scissor_origin != origin || state.scissor_dimensions != dimensions)
	{
		state.scissor_origin = origin;
		state.scissor_dimensions = dimensions;
		render_interface->SetScissorRegion(origin.x, origin.y, dimensions.x, dimensions.y);
	}
}

void RenderState::SetClipMask(ElementClipList in_clip_elements)
{
	State& state = stack.back();
	if (state.clip_stencil_elements != in_clip_elements)
	{
		state.clip_stencil_elements = std::move(in_clip_elements);
		ApplyClipMask(state.clip_stencil_elements);
	}
}

void RenderState::SetTransform(const Matrix4f* p_new_transform)
{
	State& state = stack.back();
	const Matrix4f*& p_active_transform = state.transform_pointer;

	// Only changed transforms are submitted.
	if (p_active_transform != p_new_transform)
	{
		Matrix4f& active_transform = state.transform;

		// Do a deep comparison as well to avoid submitting a new transform which is equal.
		if (!p_active_transform || !p_new_transform || (active_transform != *p_new_transform))
		{
			render_interface->SetTransform(p_new_transform);

			if (p_new_transform)
				active_transform = *p_new_transform;
		}

		p_active_transform = p_new_transform;
	}
}

void RenderState::ApplyTransform(Element* element)
{
	const Matrix4f* new_transform = nullptr;
	if (element)
	{
		if (const TransformState* transform_state = element->GetTransformState())
			new_transform = transform_state->GetTransform();
	}

	SetTransform(new_transform);
}

bool RenderState::GetScissorState(Vector2i& out_scissor_origin, Vector2i& out_scissor_dimensions) const
{
	const State& state = stack.back();
	const bool scissor_enabled = (state.scissor_dimensions.x >= 0);
	if (!scissor_enabled)
		return false;

	out_scissor_origin = state.scissor_origin;
	out_scissor_dimensions = state.scissor_dimensions;
	return true;
}

void RenderState::ApplyClipMask(const ElementClipList& clip_elements)
{
	const bool clip_mask_enabled = !clip_elements.empty();
	render_interface->EnableClipMask(clip_mask_enabled);

	if (clip_mask_enabled)
	{
		const Matrix4f* initial_transform = stack.back().transform_pointer;

		bool first_clip_mask = true;
		for (const ElementClip& element_clip : clip_elements)
		{
			const Box::Area clip_area = element_clip.clip_area;
			Element* clip_element = element_clip.element;
			const Box& box = clip_element->GetBox();
			const ComputedValues& computed = clip_element->GetComputedValues();
			const Vector4f border_radius = computed.border_radius();

			ApplyTransform(clip_element);

			// TODO: Store everything needed in ElementClip: It should not be our responsibility to probe into the element class or computed values.
			// Is it sufficient to store a pointer to the geometry and transform? Can then remove clip area.
			// @performance: Store clipping geometry on element.
			Geometry geometry(render_interface);
			GeometryUtilities::GenerateBackground(&geometry, box, {}, border_radius, Colourb(255), clip_area);

			const ClipMask clip_mask = (first_clip_mask ? ClipMask::Clip : ClipMask::ClipIntersect);
			geometry.SetClipMask(clip_mask, clip_element->GetAbsoluteOffset(Box::BORDER));
			first_clip_mask = false;
		}

		// Apply the initially set transform in case it was changed.
		// TODO: Is it safe to dereference this old pointer?
		SetTransform(initial_transform);
	}
}

void RenderState::Push()
{
	stack.push_back(State(stack.back()));
}

void RenderState::Pop()
{
	if (stack.size() >= 2)
	{
		const State& next = *(stack.end() - 2);
		Set(next);
		stack.pop_back();
	}
	else
	{
		RMLUI_ERRORMSG("Unbalanced render state push/pop.");
	}
}

void RenderState::Set(const State& next)
{
	const bool scissor_enable = (next.scissor_dimensions.x >= 0);
	if (scissor_enable)
		EnableScissorRegion(next.scissor_origin, next.scissor_dimensions);
	else
		DisableScissorRegion();

	SetClipMask(next.clip_stencil_elements);

	// TODO: Is it safe to submit an old pointer here (e.g. in case of Pop())?
	SetTransform(next.transform_pointer);
}

} // namespace Rml