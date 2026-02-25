/*
 * recast-preview-widget.cpp -- Extracted live preview widget.
 *
 * Interactive drag/resize, selection overlay, draw callback with safe refs.
 * Standalone widget used by the vertical preview dock.
 */

#include "recast-preview-widget.h"

#include <QMouseEvent>
#include <QWindow>

#include <cmath>

extern "C" {
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
}

/* ---- Helpers ---- */

static inline void GetScaleAndCenterPos(int baseCX, int baseCY, int windowCX,
					int windowCY, int &x, int &y,
					float &scale)
{
	double windowAspect = double(windowCX) / double(windowCY);
	double baseAspect = double(baseCX) / double(baseCY);
	int newCX, newCY;

	if (windowAspect > baseAspect) {
		scale = float(windowCY) / float(baseCY);
		newCX = int(double(windowCY) * baseAspect);
		newCY = windowCY;
	} else {
		scale = float(windowCX) / float(baseCX);
		newCX = windowCX;
		newCY = int(float(windowCX) / baseAspect);
	}

	x = windowCX / 2 - newCX / 2;
	y = windowCY / 2 - newCY / 2;
}

/* ---- Constructor / Destructor ---- */

RecastPreviewWidget::RecastPreviewWidget(QWidget *parent)
	: QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_NativeWindow);

	setMouseTracking(true);
	setFocusPolicy(Qt::ClickFocus);

	setMinimumHeight(200);
	setMaximumHeight(400);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

RecastPreviewWidget::~RecastPreviewWidget()
{
	if (display) {
		obs_display_remove_draw_callback(display, DrawCallback, this);
		obs_display_destroy(display);
		display = nullptr;
	}
	if (selected_item) {
		obs_sceneitem_release(selected_item);
		selected_item = nullptr;
	}
	if (scene_source) {
		obs_source_release(scene_source);
		scene_source = nullptr;
	}
}

void RecastPreviewWidget::CreateDisplay()
{
	if (display)
		return;

	if (!windowHandle() || !windowHandle()->isExposed())
		return;

	QSize size = this->size() * this->devicePixelRatioF();

	gs_init_data info = {};
	info.cx = size.width();
	info.cy = size.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;

#ifdef _WIN32
	info.window.hwnd = (HWND)windowHandle()->winId();
#elif __APPLE__
	info.window.view = (id)windowHandle()->winId();
#else
	info.window.id = windowHandle()->winId();
	info.window.display = obs_get_nix_platform_display();
#endif

	display = obs_display_create(&info, 0xFF2D2D2D);
	obs_display_add_draw_callback(display, DrawCallback, this);
}

void RecastPreviewWidget::SetScene(obs_source_t *scene, int w, int h)
{
	if (scene_source) {
		obs_source_release(scene_source);
		scene_source = nullptr;
	}
	if (scene) {
		scene_source = obs_source_get_ref(scene);
	}
	canvas_width = w;
	canvas_height = h;
}

void RecastPreviewWidget::ClearScene()
{
	if (scene_source) {
		obs_source_release(scene_source);
		scene_source = nullptr;
	}
	canvas_width = 0;
	canvas_height = 0;
	if (selected_item) {
		obs_sceneitem_release(selected_item);
		selected_item = nullptr;
	}
	interactive_scene = nullptr;
}

void RecastPreviewWidget::SetInteractiveScene(obs_scene_t *scene)
{
	interactive_scene = scene;
	if (selected_item) {
		obs_sceneitem_release(selected_item);
		selected_item = nullptr;
	}
	dragging = false;
}

void RecastPreviewWidget::ClearInteractiveScene()
{
	interactive_scene = nullptr;
	if (selected_item) {
		obs_sceneitem_release(selected_item);
		selected_item = nullptr;
	}
	dragging = false;
}

void RecastPreviewWidget::SetSelectedItem(obs_sceneitem_t *item)
{
	if (selected_item)
		obs_sceneitem_release(selected_item);
	selected_item = item;
	if (selected_item)
		obs_sceneitem_addref(selected_item);
}

/* ---- Coordinate conversion ---- */

QPointF RecastPreviewWidget::WidgetToScene(QPoint pos)
{
	if (canvas_width <= 0 || canvas_height <= 0)
		return QPointF(0, 0);

	float dpr = (float)devicePixelRatioF();
	int widgetCX = (int)(width() * dpr);
	int widgetCY = (int)(height() * dpr);

	int vx, vy;
	float scale;
	GetScaleAndCenterPos(canvas_width, canvas_height,
			     widgetCX, widgetCY, vx, vy, scale);

	if (scale <= 0.0f)
		return QPointF(0, 0);

	float scene_x = (pos.x() * dpr - vx) / scale;
	float scene_y = (pos.y() * dpr - vy) / scale;
	return QPointF(scene_x, scene_y);
}

/* ---- Item bounding rect (no rotation) ---- */

void RecastPreviewWidget::GetItemRect(obs_sceneitem_t *item,
				      float &rx, float &ry,
				      float &rw, float &rh)
{
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src) {
		rx = ry = rw = rh = 0;
		return;
	}
	float cx = (float)obs_source_get_width(src);
	float cy = (float)obs_source_get_height(src);

	struct vec2 pos, sc;
	obs_sceneitem_get_pos(item, &pos);
	obs_sceneitem_get_scale(item, &sc);

	struct obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);

	float crop_cx = cx - (float)(crop.left + crop.right);
	float crop_cy = cy - (float)(crop.top + crop.bottom);
	if (crop_cx < 1.0f) crop_cx = 1.0f;
	if (crop_cy < 1.0f) crop_cy = 1.0f;

	rx = pos.x;
	ry = pos.y;
	rw = crop_cx * sc.x;
	rh = crop_cy * sc.y;
}

/* ---- Hit testing ---- */

struct hit_test_ctx {
	float mx, my;
	obs_sceneitem_t *result;
};

static bool hit_test_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<struct hit_test_ctx *>(param);

	if (!obs_sceneitem_visible(item))
		return true;

	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return true;
	float cx = (float)obs_source_get_width(src);
	float cy = (float)obs_source_get_height(src);
	if (cx <= 0 || cy <= 0)
		return true;

	/* Use box transform + inverse for accurate hit test */
	struct matrix4 box_transform;
	obs_sceneitem_get_box_transform(item, &box_transform);

	struct matrix4 inv;
	matrix4_inv(&inv, &box_transform);

	struct vec3 scene_pt;
	vec3_set(&scene_pt, ctx->mx, ctx->my, 0.0f);

	struct vec3 local_pt;
	vec3_transform(&local_pt, &scene_pt, &inv);

	if (local_pt.x >= 0.0f && local_pt.x <= cx &&
	    local_pt.y >= 0.0f && local_pt.y <= cy) {
		ctx->result = item;
	}

	return true; /* continue to find topmost */
}

obs_sceneitem_t *RecastPreviewWidget::HitTestItems(float scene_x,
						    float scene_y)
{
	if (!interactive_scene)
		return nullptr;

	struct hit_test_ctx ctx;
	ctx.mx = scene_x;
	ctx.my = scene_y;
	ctx.result = nullptr;

	obs_scene_enum_items(interactive_scene, hit_test_cb, &ctx);
	return ctx.result;
}

int RecastPreviewWidget::HitTestHandles(float scene_x, float scene_y)
{
	if (!selected_item)
		return HANDLE_NONE;

	float rx, ry, rw, rh;
	GetItemRect(selected_item, rx, ry, rw, rh);

	/* Handle size in scene coords -- scale-independent 8px */
	float dpr = (float)devicePixelRatioF();
	int widgetCX = (int)(width() * dpr);
	int widgetCY = (int)(height() * dpr);
	int vx_unused, vy_unused;
	float view_scale;
	GetScaleAndCenterPos(canvas_width, canvas_height,
			     widgetCX, widgetCY, vx_unused, vy_unused,
			     view_scale);
	float hr = (view_scale > 0.0f) ? (8.0f * dpr / view_scale) : 8.0f;

	/* 8 handle positions */
	struct { float x, y; } handles[8] = {
		{rx,          ry},           /* TL */
		{rx + rw / 2, ry},           /* T  */
		{rx + rw,     ry},           /* TR */
		{rx + rw,     ry + rh / 2},  /* R  */
		{rx + rw,     ry + rh},      /* BR */
		{rx + rw / 2, ry + rh},      /* B  */
		{rx,          ry + rh},      /* BL */
		{rx,          ry + rh / 2},  /* L  */
	};

	for (int i = 0; i < 8; i++) {
		float dx = scene_x - handles[i].x;
		float dy = scene_y - handles[i].y;
		if (std::fabs(dx) <= hr && std::fabs(dy) <= hr)
			return i;
	}

	/* Check body */
	if (scene_x >= rx && scene_x <= rx + rw &&
	    scene_y >= ry && scene_y <= ry + rh)
		return HANDLE_BODY;

	return HANDLE_NONE;
}

/* ---- Mouse events ---- */

void RecastPreviewWidget::mousePressEvent(QMouseEvent *event)
{
	if (!interactive_scene || event->button() != Qt::LeftButton) {
		QWidget::mousePressEvent(event);
		return;
	}

	QPointF sp = WidgetToScene(event->pos());
	float sx = (float)sp.x();
	float sy = (float)sp.y();

	/* First check if clicking on a handle of the selected item */
	int handle = HitTestHandles(sx, sy);

	if (handle != HANDLE_NONE && selected_item) {
		/* Start dragging a handle or body */
		dragging = true;
		drag_handle = handle;
		drag_start_mouse = sp;

		struct vec2 pos, sc;
		obs_sceneitem_get_pos(selected_item, &pos);
		obs_sceneitem_get_scale(selected_item, &sc);
		item_start_pos_x = pos.x;
		item_start_pos_y = pos.y;
		item_start_scale_x = sc.x;
		item_start_scale_y = sc.y;

		obs_source_t *src = obs_sceneitem_get_source(selected_item);
		if (!src) return;
		struct obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(selected_item, &crop);
		item_start_width = ((float)obs_source_get_width(src) -
				    (float)(crop.left + crop.right));
		item_start_height = ((float)obs_source_get_height(src) -
				     (float)(crop.top + crop.bottom));
		if (item_start_width < 1.0f) item_start_width = 1.0f;
		if (item_start_height < 1.0f) item_start_height = 1.0f;
		return;
	}

	/* Hit test scene items */
	obs_sceneitem_t *hit = HitTestItems(sx, sy);
	if (selected_item)
		obs_sceneitem_release(selected_item);
	selected_item = hit;
	if (selected_item)
		obs_sceneitem_addref(selected_item);
	emit itemSelected(selected_item);

	if (hit) {
		dragging = true;
		drag_handle = HANDLE_BODY;
		drag_start_mouse = sp;

		struct vec2 pos, sc;
		obs_sceneitem_get_pos(hit, &pos);
		obs_sceneitem_get_scale(hit, &sc);
		item_start_pos_x = pos.x;
		item_start_pos_y = pos.y;
		item_start_scale_x = sc.x;
		item_start_scale_y = sc.y;

		obs_source_t *src = obs_sceneitem_get_source(hit);
		if (!src) return;
		struct obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(hit, &crop);
		item_start_width = ((float)obs_source_get_width(src) -
				    (float)(crop.left + crop.right));
		item_start_height = ((float)obs_source_get_height(src) -
				     (float)(crop.top + crop.bottom));
		if (item_start_width < 1.0f) item_start_width = 1.0f;
		if (item_start_height < 1.0f) item_start_height = 1.0f;
	}
}

void RecastPreviewWidget::mouseMoveEvent(QMouseEvent *event)
{
	if (!interactive_scene) {
		QWidget::mouseMoveEvent(event);
		return;
	}

	QPointF sp = WidgetToScene(event->pos());
	float sx = (float)sp.x();
	float sy = (float)sp.y();

	/* Update cursor based on handle hover */
	if (!dragging && selected_item) {
		int handle = HitTestHandles(sx, sy);
		switch (handle) {
		case HANDLE_TL: case HANDLE_BR:
			setCursor(Qt::SizeFDiagCursor); break;
		case HANDLE_TR: case HANDLE_BL:
			setCursor(Qt::SizeBDiagCursor); break;
		case HANDLE_T: case HANDLE_B:
			setCursor(Qt::SizeVerCursor); break;
		case HANDLE_L: case HANDLE_R:
			setCursor(Qt::SizeHorCursor); break;
		case HANDLE_BODY:
			setCursor(Qt::SizeAllCursor); break;
		default:
			setCursor(Qt::ArrowCursor); break;
		}
	}

	if (!dragging || !selected_item)
		return;

	float dx = sx - (float)drag_start_mouse.x();
	float dy = sy - (float)drag_start_mouse.y();

	if (drag_handle == HANDLE_BODY) {
		/* Move */
		struct vec2 new_pos;
		new_pos.x = item_start_pos_x + dx;
		new_pos.y = item_start_pos_y + dy;
		obs_sceneitem_set_pos(selected_item, &new_pos);

	} else {
		/* Resize via handles */
		float new_pos_x = item_start_pos_x;
		float new_pos_y = item_start_pos_y;
		float new_sx = item_start_scale_x;
		float new_sy = item_start_scale_y;

		float orig_w = item_start_width * item_start_scale_x;
		float orig_h = item_start_height * item_start_scale_y;

		switch (drag_handle) {
		case HANDLE_BR:
			new_sx = (orig_w + dx) / item_start_width;
			new_sy = (orig_h + dy) / item_start_height;
			break;
		case HANDLE_R:
			new_sx = (orig_w + dx) / item_start_width;
			break;
		case HANDLE_B:
			new_sy = (orig_h + dy) / item_start_height;
			break;
		case HANDLE_TL:
			new_pos_x = item_start_pos_x + dx;
			new_pos_y = item_start_pos_y + dy;
			new_sx = (orig_w - dx) / item_start_width;
			new_sy = (orig_h - dy) / item_start_height;
			break;
		case HANDLE_T:
			new_pos_y = item_start_pos_y + dy;
			new_sy = (orig_h - dy) / item_start_height;
			break;
		case HANDLE_L:
			new_pos_x = item_start_pos_x + dx;
			new_sx = (orig_w - dx) / item_start_width;
			break;
		case HANDLE_TR:
			new_pos_y = item_start_pos_y + dy;
			new_sx = (orig_w + dx) / item_start_width;
			new_sy = (orig_h - dy) / item_start_height;
			break;
		case HANDLE_BL:
			new_pos_x = item_start_pos_x + dx;
			new_sx = (orig_w - dx) / item_start_width;
			new_sy = (orig_h + dy) / item_start_height;
			break;
		}

		/* Clamp to minimum */
		if (new_sx < 0.01f) new_sx = 0.01f;
		if (new_sy < 0.01f) new_sy = 0.01f;

		struct vec2 pos_v = {new_pos_x, new_pos_y};
		struct vec2 sc_v = {new_sx, new_sy};
		obs_sceneitem_set_pos(selected_item, &pos_v);
		obs_sceneitem_set_scale(selected_item, &sc_v);
	}

	emit itemTransformed();
}

void RecastPreviewWidget::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton && dragging) {
		dragging = false;
		drag_handle = HANDLE_NONE;
		if (selected_item)
			emit itemTransformed();
	}
	QWidget::mouseReleaseEvent(event);
}

/* ---- Selection overlay drawing ---- */

void RecastPreviewWidget::DrawSelectionOverlay(RecastPreviewWidget *widget)
{
	obs_sceneitem_t *item = widget->selected_item;
	if (!item)
		return;

	/* Validate the sceneitem's source is still alive */
	obs_source_t *item_src = obs_sceneitem_get_source(item);
	if (!item_src)
		return;

	float rx, ry, rw, rh;
	widget->GetItemRect(item, rx, ry, rw, rh);
	if (rw <= 0 || rh <= 0)
		return;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color_param =
		gs_effect_get_param_by_name(solid, "color");

	/* Selection rectangle: green */
	struct vec4 green;
	vec4_set(&green, 0.0f, 1.0f, 0.0f, 1.0f);
	gs_effect_set_vec4(color_param, &green);

	while (gs_effect_loop(solid, "Solid")) {
		/* Draw bounding box outline */
		gs_render_start(true);
		gs_vertex2f(rx, ry);
		gs_vertex2f(rx + rw, ry);
		gs_vertex2f(rx + rw, ry + rh);
		gs_vertex2f(rx, ry + rh);
		gs_vertex2f(rx, ry);
		gs_render_stop(GS_LINESTRIP);
	}

	/* Handle squares: white filled */
	struct vec4 white;
	vec4_set(&white, 1.0f, 1.0f, 1.0f, 1.0f);
	gs_effect_set_vec4(color_param, &white);

	float hr = 4.0f; /* handle half-size in scene coords */
	/* Scale handle size based on canvas so they look reasonable */
	if (widget->canvas_width > 0)
		hr = (float)widget->canvas_width / 240.0f;
	if (hr < 3.0f) hr = 3.0f;
	if (hr > 12.0f) hr = 12.0f;

	struct { float x, y; } handles[8] = {
		{rx,          ry},
		{rx + rw / 2, ry},
		{rx + rw,     ry},
		{rx + rw,     ry + rh / 2},
		{rx + rw,     ry + rh},
		{rx + rw / 2, ry + rh},
		{rx,          ry + rh},
		{rx,          ry + rh / 2},
	};

	while (gs_effect_loop(solid, "Solid")) {
		for (int i = 0; i < 8; i++) {
			float hx = handles[i].x;
			float hy = handles[i].y;
			gs_render_start(true);
			gs_vertex2f(hx - hr, hy - hr);
			gs_vertex2f(hx + hr, hy - hr);
			gs_vertex2f(hx - hr, hy + hr);
			gs_vertex2f(hx + hr, hy + hr);
			gs_render_stop(GS_TRISTRIP);
		}
	}

	/* Handle outlines: dark border */
	struct vec4 dark;
	vec4_set(&dark, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_effect_set_vec4(color_param, &dark);

	while (gs_effect_loop(solid, "Solid")) {
		for (int i = 0; i < 8; i++) {
			float hx = handles[i].x;
			float hy = handles[i].y;
			gs_render_start(true);
			gs_vertex2f(hx - hr, hy - hr);
			gs_vertex2f(hx + hr, hy - hr);
			gs_vertex2f(hx + hr, hy + hr);
			gs_vertex2f(hx - hr, hy + hr);
			gs_vertex2f(hx - hr, hy - hr);
			gs_render_stop(GS_LINESTRIP);
		}
	}
}

void RecastPreviewWidget::paintEvent(QPaintEvent *)
{
	if (!display)
		CreateDisplay();
}

void RecastPreviewWidget::resizeEvent(QResizeEvent *)
{
	if (display) {
		QSize size = this->size() * this->devicePixelRatioF();
		obs_display_resize(display, size.width(), size.height());
	}
}

void RecastPreviewWidget::DrawCallback(void *param, uint32_t cx, uint32_t cy)
{
	auto *widget = static_cast<RecastPreviewWidget *>(param);
	if (!widget->scene_source || widget->canvas_width <= 0 ||
	    widget->canvas_height <= 0)
		return;

	/* Take a safe reference to the source for rendering.
	 * This prevents a crash if the source is freed on the UI thread
	 * while we are rendering on the graphics thread. */
	obs_source_t *src = obs_source_get_ref(widget->scene_source);
	if (!src)
		return;

	int x, y;
	float scale;
	GetScaleAndCenterPos(widget->canvas_width, widget->canvas_height,
			     (int)cx, (int)cy, x, y, scale);

	int newCX = int(scale * float(widget->canvas_width));
	int newCY = int(scale * float(widget->canvas_height));

	gs_viewport_push();
	gs_projection_push();
	gs_set_viewport(x, y, newCX, newCY);
	gs_ortho(0.0f, float(widget->canvas_width), 0.0f,
		 float(widget->canvas_height), -100.0f, 100.0f);

	obs_source_video_render(src);

	/* Draw selection overlay if interactive */
	if (widget->interactive_scene && widget->selected_item)
		DrawSelectionOverlay(widget);

	gs_projection_pop();
	gs_viewport_pop();

	obs_source_release(src);
}
