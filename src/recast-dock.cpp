/*
 * recast-dock.cpp -- Qt dock widget UI for Recast multi-output.
 *
 * Provides the "Recast Output" panel in OBS with:
 *   - Output cards (start/stop/delete per target)
 *   - "Add Output" dialog (with independent scenes option)
 *   - "Sync with Recast Server" button
 *   - Periodic status refresh
 *   - Per-output dock management via RecastDockManager
 */

#include "recast-dock.h"
#include "recast-dock-manager.h"
#include "recast-platform-icons.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMouseEvent>
#include <QScrollArea>
#include <QWindow>

#include <cmath>

extern "C" {
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <util/platform.h>
#include <util/dstr.h>
#include "recast-protocol.h"
}

/* ====================================================================
 * RecastAddDialog
 * ==================================================================== */

RecastAddDialog::RecastAddDialog(QWidget *parent)
	: QDialog(parent)
{
	setWindowTitle(obs_module_text("Recast.AddOutput"));
	setMinimumWidth(400);

	auto *form = new QFormLayout;

	name_edit = new QLineEdit;
	name_edit->setPlaceholderText("e.g. Twitch, YouTube Shorts");
	form->addRow(obs_module_text("Recast.Name"), name_edit);

	url_edit = new QLineEdit;
	url_edit->setPlaceholderText("rtmp://...");
	form->addRow(obs_module_text("Recast.URL"), url_edit);

	key_edit = new QLineEdit;
	key_edit->setEchoMode(QLineEdit::Password);
	key_edit->setPlaceholderText("Stream key");
	form->addRow(obs_module_text("Recast.Key"), key_edit);

	scene_combo = new QComboBox;
	scene_combo->addItem(obs_module_text("Recast.MainScene"), "");
	populateScenes();
	form->addRow(obs_module_text("Recast.Scene"), scene_combo);

	/* Resolution override (0 = use main canvas) */
	auto *res_layout = new QHBoxLayout;
	width_spin = new QSpinBox;
	width_spin->setRange(0, 7680);
	width_spin->setSpecialValueText(obs_module_text("Recast.Auto"));
	height_spin = new QSpinBox;
	height_spin->setRange(0, 7680);
	height_spin->setSpecialValueText(obs_module_text("Recast.Auto"));
	res_layout->addWidget(width_spin);
	res_layout->addWidget(new QLabel("x"));
	res_layout->addWidget(height_spin);
	form->addRow(obs_module_text("Recast.Resolution"), res_layout);

	/* Independent scenes checkbox */
	private_scenes_check = new QCheckBox(
		obs_module_text("Recast.UsePrivateScenes"));
	private_scenes_check->setChecked(true);
	private_scenes_check->setToolTip(
		obs_module_text("Recast.UsePrivateScenes.Tip"));
	form->addRow("", private_scenes_check);

	/* When independent scenes is checked, disable legacy scene combo */
	connect(private_scenes_check, &QCheckBox::toggled, this,
		[this](bool checked) {
			scene_combo->setEnabled(!checked);
		});
	scene_combo->setEnabled(false); /* default: checked */

	auto *buttons =
		new QDialogButtonBox(QDialogButtonBox::Ok |
				     QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *root = new QVBoxLayout(this);
	root->addLayout(form);
	root->addWidget(buttons);
}

void RecastAddDialog::populateScenes()
{
	char **scene_names = obs_frontend_get_scene_names();
	if (!scene_names)
		return;
	for (char **s = scene_names; *s; s++) {
		scene_combo->addItem(QString::fromUtf8(*s),
				     QString::fromUtf8(*s));
	}
	bfree(scene_names);
}

QString RecastAddDialog::getName() const { return name_edit->text(); }
QString RecastAddDialog::getUrl() const { return url_edit->text(); }
QString RecastAddDialog::getKey() const { return key_edit->text(); }

QString RecastAddDialog::getScene() const
{
	return scene_combo->currentData().toString();
}

int RecastAddDialog::getWidth() const { return width_spin->value(); }
int RecastAddDialog::getHeight() const { return height_spin->value(); }

bool RecastAddDialog::getUsePrivateScenes() const
{
	return private_scenes_check->isChecked();
}

/* ====================================================================
 * RecastPreviewWidget
 * ==================================================================== */

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

	/* Handle size in scene coords — scale-independent 8px */
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

/* ====================================================================
 * RecastOutputCard
 * ==================================================================== */

RecastOutputCard::RecastOutputCard(recast_output_target_t *target,
				   QWidget *parent)
	: QGroupBox(parent), target_(target)
{
	setTitle(QString::fromUtf8(target->name));

	auto *vbox = new QVBoxLayout(this);

	/* Title row: platform icon + URL + protocol badge */
	auto *title_row = new QHBoxLayout;

	/* Platform icon */
	QString platform_id = recast_detect_platform(
		QString::fromUtf8(target->url));
	platform_icon_label = new QLabel;
	platform_icon_label->setFixedSize(20, 20);
	if (!platform_id.isEmpty()) {
		QIcon icon = recast_platform_icon(platform_id);
		platform_icon_label->setPixmap(
			icon.pixmap(20, 20));
	}
	title_row->addWidget(platform_icon_label);

	/* URL (masked) */
	QString masked_url = QString::fromUtf8(target->url);
	if (masked_url.length() > 30)
		masked_url = masked_url.left(27) + "...";
	title_row->addWidget(new QLabel(masked_url), 1);

	/* Protocol badge */
	protocol_label = new QLabel(
		QString::fromUtf8(recast_protocol_name(target->protocol)));
	protocol_label->setStyleSheet(
		"background: #444; color: #fff; padding: 1px 4px; "
		"border-radius: 3px; font-size: 10px;");
	title_row->addWidget(protocol_label);

	vbox->addLayout(title_row);

	/* Scene row */
	QString scene_text;
	if (target->use_private_scenes) {
		scene_text = obs_module_text("Recast.IndependentScenes");
	} else if (target->scene_name && *target->scene_name) {
		scene_text = QString("Scene: %1").arg(
			QString::fromUtf8(target->scene_name));
	} else {
		scene_text = QString("Scene: (main)");
	}
	vbox->addWidget(new QLabel(scene_text));

	/* Resolution row (if custom) */
	if (target->width > 0 && target->height > 0) {
		vbox->addWidget(new QLabel(
			QString("Resolution: %1x%2")
				.arg(target->width)
				.arg(target->height)));
	}

	/* Encoding mode row */
	QString enc_text;
	if (target->advanced_encoder) {
		enc_text = QString("Encoder: %1 @ %2 kbps")
				   .arg(target->encoder_id
						? QString::fromUtf8(
							  target->encoder_id)
						: "obs_x264")
				   .arg(target->custom_bitrate > 0
						? target->custom_bitrate
						: 4000);
	} else {
		enc_text = "Encoder: Shared (zero overhead)";
	}
	vbox->addWidget(new QLabel(enc_text));

	/* Status + buttons row */
	auto *bottom = new QHBoxLayout;

	status_label = new QLabel(obs_module_text("Recast.Status.Stopped"));
	bottom->addWidget(status_label, 1);

	/* Auto checkbox */
	auto_check = new QCheckBox(obs_module_text("Recast.Auto"));
	auto_check->setChecked(target->auto_start);
	auto_check->setToolTip("Auto start/stop with main stream");
	connect(auto_check, &QCheckBox::toggled, this,
		[this](bool checked) {
			target_->auto_start = checked;
			target_->auto_stop = checked;
			emit autoChanged(this);
		});
	bottom->addWidget(auto_check);

	toggle_btn = new QPushButton(obs_module_text("Recast.Start"));
	toggle_btn->setFixedWidth(60);
	connect(toggle_btn, &QPushButton::clicked,
		this, &RecastOutputCard::onToggleStream);
	bottom->addWidget(toggle_btn);

	delete_btn = new QPushButton(obs_module_text("Recast.Delete"));
	delete_btn->setFixedWidth(30);
	delete_btn->setToolTip(obs_module_text("Recast.DeleteTip"));
	connect(delete_btn, &QPushButton::clicked, this, [this]() {
		emit deleteRequested(this);
	});
	bottom->addWidget(delete_btn);

	vbox->addLayout(bottom);
}

RecastOutputCard::~RecastOutputCard()
{
	/* The target is owned by the dock / card lifecycle */
}

void RecastOutputCard::refreshStatus()
{
	if (!target_)
		return;

	if (target_->active) {
		uint64_t sec = recast_output_target_elapsed_sec(target_);
		uint64_t h = sec / 3600;
		uint64_t m = (sec % 3600) / 60;
		uint64_t s = sec % 60;

		QString time_str;
		if (h > 0)
			time_str = QString("%1h %2m").arg(h).arg(m);
		else
			time_str = QString("%1m %2s").arg(m).arg(s);

		status_label->setText(
			QString("%1 (%2)")
				.arg(obs_module_text("Recast.Status.Streaming"))
				.arg(time_str));
		status_label->setStyleSheet("color: #4CAF50; font-weight: bold;");
		toggle_btn->setText(obs_module_text("Recast.Stop"));
	} else {
		status_label->setText(
			obs_module_text("Recast.Status.Stopped"));
		status_label->setStyleSheet("color: #999;");
		toggle_btn->setText(obs_module_text("Recast.Start"));
	}
}

void RecastOutputCard::onToggleStream()
{
	if (!target_)
		return;

	if (target_->active) {
		recast_output_target_stop(target_);
	} else {
		if (!recast_output_target_start(target_)) {
			QMessageBox::warning(
				this, obs_module_text("Recast.Error"),
				obs_module_text("Recast.Error.StartFailed"));
		}
	}
	refreshStatus();
}

void RecastOutputCard::setSelected(bool sel)
{
	selected_ = sel;
	if (sel) {
		setStyleSheet("RecastOutputCard { border: 2px solid #4CAF50; }");
	} else {
		setStyleSheet("");
	}
}

void RecastOutputCard::mousePressEvent(QMouseEvent *event)
{
	QGroupBox::mousePressEvent(event);
	emit clicked(this);
}

/* ====================================================================
 * RecastDock
 * ==================================================================== */

RecastDock::RecastDock(QWidget *parent)
	: QDockWidget(obs_module_text("Recast.DockTitle"), parent)
{
	setObjectName("RecastOutputDock");
	setFeatures(QDockWidget::DockWidgetMovable |
		    QDockWidget::DockWidgetFloatable);

	/* Create dock manager for per-output docks */
	dock_manager_ = new RecastDockManager(this);
	connect(dock_manager_, &RecastDockManager::configChanged,
		this, &RecastDock::onConfigChanged);

	/* Scroll area for output cards */
	auto *scroll = new QScrollArea;
	scroll->setWidgetResizable(true);

	auto *container = new QWidget;
	auto *root_layout = new QVBoxLayout(container);
	root_layout->setAlignment(Qt::AlignTop);

	/* Add Output button */
	auto *add_btn =
		new QPushButton(obs_module_text("Recast.AddOutput"));
	connect(add_btn, &QPushButton::clicked,
		this, &RecastDock::onAddOutput);
	root_layout->addWidget(add_btn);

	/* Cards container */
	cards_layout = new QVBoxLayout;
	cards_layout->setAlignment(Qt::AlignTop);
	root_layout->addLayout(cards_layout);

	/* Preview label (for non-private-scenes outputs) */
	preview_label = new QLabel(
		obs_module_text("Recast.PreviewNone"));
	preview_label->setStyleSheet("color: #999; padding: 4px 0;");
	root_layout->addWidget(preview_label);

	/* Live preview widget (for non-private-scenes outputs) */
	preview = new RecastPreviewWidget;
	root_layout->addWidget(preview);

	/* Stretch to push sync button to bottom */
	root_layout->addStretch();

	/* Sync with Recast Server button */
	auto *sync_btn =
		new QPushButton(obs_module_text("Recast.SyncServer"));
	connect(sync_btn, &QPushButton::clicked,
		this, &RecastDock::onSyncServer);
	root_layout->addWidget(sync_btn);

	scroll->setWidget(container);
	setWidget(scroll);

	/* Network manager for API calls */
	net_mgr = new QNetworkAccessManager(this);

	/* Refresh timer (every second for elapsed time display) */
	refresh_timer = new QTimer(this);
	connect(refresh_timer, &QTimer::timeout,
		this, &RecastDock::onRefreshTimer);
	refresh_timer->start(1000);

	/* Register for main stream start/stop events (auto start/stop) */
	obs_frontend_add_event_callback(onFrontendEvent, this);

	/* Load saved config */
	loadFromConfig();
}

RecastDock::~RecastDock()
{
	refresh_timer->stop();

	obs_frontend_remove_event_callback(onFrontendEvent, this);

	/* Destroy all per-output docks */
	dock_manager_->destroyAll();

	/* Stop all active outputs and destroy targets */
	for (auto *card : cards) {
		recast_output_target_t *t = card->target();
		if (t) {
			recast_output_target_destroy(t);
		}
	}
	cards.clear();
}

void RecastDock::onAddOutput()
{
	RecastAddDialog dlg(this);
	if (dlg.exec() != QDialog::Accepted)
		return;

	QString name = dlg.getName().trimmed();
	QString url = dlg.getUrl().trimmed();
	QString key = dlg.getKey().trimmed();

	if (name.isEmpty() || url.isEmpty()) {
		QMessageBox::warning(
			this, obs_module_text("Recast.Error"),
			obs_module_text("Recast.Error.NameUrlRequired"));
		return;
	}

	recast_output_target_t *target = recast_output_target_create(
		name.toUtf8().constData(),
		url.toUtf8().constData(),
		key.toUtf8().constData(),
		dlg.getScene().toUtf8().constData(),
		dlg.getWidth(), dlg.getHeight());

	/* Set up independent scenes if requested */
	target->use_private_scenes = dlg.getUsePrivateScenes();
	if (target->use_private_scenes) {
		target->scene_model = recast_scene_model_create();
	}

	addCard(target);
	saveToConfig();
}

void RecastDock::onDeleteOutput(RecastOutputCard *card)
{
	if (!card)
		return;

	auto answer = QMessageBox::question(
		this, obs_module_text("Recast.Confirm"),
		QString("%1 '%2'?")
			.arg(obs_module_text("Recast.ConfirmDelete"))
			.arg(card->target()->name));

	if (answer != QMessageBox::Yes)
		return;

	removeCard(card);
	saveToConfig();
}

void RecastDock::onSyncServer()
{
	char *token = recast_config_get_server_token();
	if (!token || !*token) {
		bfree(token);

		/* Prompt for token */
		QLineEdit *input = new QLineEdit;
		input->setPlaceholderText("Recast API token");

		QDialog dlg(this);
		dlg.setWindowTitle(obs_module_text("Recast.SyncServer"));
		auto *layout = new QVBoxLayout(&dlg);
		layout->addWidget(
			new QLabel(obs_module_text("Recast.EnterToken")));
		layout->addWidget(input);
		auto *btns = new QDialogButtonBox(
			QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
		connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		layout->addWidget(btns);

		if (dlg.exec() != QDialog::Accepted ||
		    input->text().trimmed().isEmpty())
			return;

		QString new_token = input->text().trimmed();
		recast_config_set_server_token(
			new_token.toUtf8().constData());
		token = bstrdup(new_token.toUtf8().constData());
	}

	/* GET /api/stream/status?token=XXX */
	QString url = QString("https://api.recast.stream/api/stream/status?token=%1")
			      .arg(QString::fromUtf8(token));
	bfree(token);

	QNetworkReply *reply = net_mgr->get(QNetworkRequest(QUrl(url)));
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			QMessageBox::warning(
				this, obs_module_text("Recast.Error"),
				QString("%1: %2")
					.arg(obs_module_text("Recast.Error.Sync"))
					.arg(reply->errorString()));
			return;
		}

		QByteArray data = reply->readAll();
		QJsonDocument doc = QJsonDocument::fromJson(data);
		if (!doc.isObject())
			return;

		QJsonArray platforms =
			doc.object().value("platforms").toArray();

		for (const QJsonValue &val : platforms) {
			QJsonObject p = val.toObject();
			QString name = p.value("name").toString();
			QString rtmp_url = p.value("rtmpUrl").toString();
			QString key = p.value("streamKey").toString();

			if (name.isEmpty() || rtmp_url.isEmpty())
				continue;

			/* Check if we already have this target */
			bool exists = false;
			for (auto *card : cards) {
				if (QString::fromUtf8(card->target()->url) ==
				    rtmp_url) {
					exists = true;
					break;
				}
			}
			if (exists)
				continue;

			recast_output_target_t *target =
				recast_output_target_create(
					name.toUtf8().constData(),
					rtmp_url.toUtf8().constData(),
					key.toUtf8().constData(),
					"", 0, 0);
			addCard(target);
		}

		saveToConfig();

		QMessageBox::information(
			this, obs_module_text("Recast.SyncServer"),
			obs_module_text("Recast.SyncComplete"));
	});
}

void RecastDock::onRefreshTimer()
{
	for (auto *card : cards)
		card->refreshStatus();
}

void RecastDock::onConfigChanged()
{
	saveToConfig();
}

void RecastDock::onFrontendEvent(enum obs_frontend_event event, void *data)
{
	RecastDock *dock = static_cast<RecastDock *>(data);
	if (!dock)
		return;

	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
		dock->onMainStreamStarted();
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
		dock->onMainStreamStopped();
	} else if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
		dock->onMainSceneChanged();
	}
}

void RecastDock::onMainStreamStarted()
{
	for (auto *card : cards) {
		recast_output_target_t *t = card->target();
		if (t && t->auto_start && !t->active) {
			if (recast_output_target_start(t)) {
				blog(LOG_INFO,
				     "[Recast] Auto-started output '%s'",
				     t->name);
			}
			card->refreshStatus();
		}
	}
}

void RecastDock::onMainStreamStopped()
{
	for (auto *card : cards) {
		recast_output_target_t *t = card->target();
		if (t && t->auto_stop && t->active) {
			recast_output_target_stop(t);
			blog(LOG_INFO,
			     "[Recast] Auto-stopped output '%s'",
			     t->name);
			card->refreshStatus();
		}
	}
}

void RecastDock::onMainSceneChanged()
{
	/* Get current main scene name */
	obs_source_t *current = obs_frontend_get_current_scene();
	if (!current)
		return;

	const char *main_scene_name = obs_source_get_name(current);

	/* For each output with scene linking, auto-switch */
	for (auto *card : cards) {
		recast_output_target_t *t = card->target();
		if (!t || !t->use_private_scenes || !t->scene_model)
			continue;

		int linked_idx = recast_scene_model_find_linked(
			t->scene_model, main_scene_name);
		if (linked_idx >= 0 &&
		    linked_idx != t->scene_model->active_scene_idx) {
			recast_scene_model_set_active(t->scene_model,
						      linked_idx);
			recast_output_target_bind_active_scene(t);
			blog(LOG_INFO,
			     "[Recast] Scene linked: '%s' -> scene %d",
			     t->name, linked_idx);
		}
	}

	obs_source_release(current);
}

void RecastDock::registerHotkeys(recast_output_target_t *target)
{
	OutputHotkeys hk = {};

	struct dstr start_name = {0};
	struct dstr stop_name = {0};
	struct dstr rec_start_name = {0};
	struct dstr rec_stop_name = {0};

	dstr_printf(&start_name, "Recast.StartStream.%s", target->name);
	dstr_printf(&stop_name, "Recast.StopStream.%s", target->name);
	dstr_printf(&rec_start_name, "Recast.StartRecord.%s", target->name);
	dstr_printf(&rec_stop_name, "Recast.StopRecord.%s", target->name);

	hk.start_stream = obs_hotkey_register_frontend(
		start_name.array,
		start_name.array,
		[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (!pressed)
				return;
			auto *t = static_cast<recast_output_target_t *>(data);
			if (!t->active)
				recast_output_target_start(t);
		},
		target);

	hk.stop_stream = obs_hotkey_register_frontend(
		stop_name.array,
		stop_name.array,
		[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (!pressed)
				return;
			auto *t = static_cast<recast_output_target_t *>(data);
			if (t->active)
				recast_output_target_stop(t);
		},
		target);

	hk.start_record = obs_hotkey_register_frontend(
		rec_start_name.array,
		rec_start_name.array,
		[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (!pressed)
				return;
			auto *t = static_cast<recast_output_target_t *>(data);
			if (!t->rec_active)
				recast_output_target_start_recording(t);
		},
		target);

	hk.stop_record = obs_hotkey_register_frontend(
		rec_stop_name.array,
		rec_stop_name.array,
		[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (!pressed)
				return;
			auto *t = static_cast<recast_output_target_t *>(data);
			if (t->rec_active)
				recast_output_target_stop_recording(t);
		},
		target);

	dstr_free(&start_name);
	dstr_free(&stop_name);
	dstr_free(&rec_start_name);
	dstr_free(&rec_stop_name);

	hotkeys_.push_back(hk);
}

void RecastDock::unregisterHotkeys(size_t index)
{
	if (index >= hotkeys_.size())
		return;

	OutputHotkeys &hk = hotkeys_[index];
	obs_hotkey_unregister(hk.start_stream);
	obs_hotkey_unregister(hk.stop_stream);
	obs_hotkey_unregister(hk.start_record);
	obs_hotkey_unregister(hk.stop_record);
}

void RecastDock::loadFromConfig()
{
	obs_data_array_t *arr = recast_config_load();
	if (!arr)
		return;

	size_t count = obs_data_array_count(arr);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);

		const char *name = obs_data_get_string(item, "name");
		const char *url = obs_data_get_string(item, "url");
		const char *key = obs_data_get_string(item, "key");
		const char *scene = obs_data_get_string(item, "scene");
		int w = (int)obs_data_get_int(item, "width");
		int h = (int)obs_data_get_int(item, "height");

		recast_output_target_t *target =
			recast_output_target_create(name, url, key, scene,
						    w, h);
		target->enabled = obs_data_get_bool(item, "enabled");
		target->auto_start = obs_data_get_bool(item, "autoStart");
		target->auto_stop = obs_data_get_bool(item, "autoStop");
		target->use_private_scenes =
			obs_data_get_bool(item, "usePrivateScenes");

		/* Encoding config */
		target->advanced_encoder =
			obs_data_get_bool(item, "advancedEncoder");
		const char *enc_id =
			obs_data_get_string(item, "encoderId");
		if (enc_id && *enc_id) {
			bfree(target->encoder_id);
			target->encoder_id = bstrdup(enc_id);
		}
		target->custom_bitrate =
			(int)obs_data_get_int(item, "customBitrate");
		target->audio_track =
			(int)obs_data_get_int(item, "audioTrack");

		obs_data_t *enc_settings =
			obs_data_get_obj(item, "encoderSettings");
		if (enc_settings) {
			if (target->encoder_settings)
				obs_data_release(target->encoder_settings);
			target->encoder_settings = enc_settings;
		}

		/* Recording config */
		target->rec_enabled =
			obs_data_get_bool(item, "recEnabled");
		const char *rec_path =
			obs_data_get_string(item, "recPath");
		if (rec_path && *rec_path) {
			bfree(target->rec_path);
			target->rec_path = bstrdup(rec_path);
		}
		const char *rec_fmt =
			obs_data_get_string(item, "recFormat");
		if (rec_fmt && *rec_fmt) {
			bfree(target->rec_format);
			target->rec_format = bstrdup(rec_fmt);
		}
		target->rec_bitrate =
			(int)obs_data_get_int(item, "recBitrate");

		/* Restore saved ID if present */
		const char *saved_id = obs_data_get_string(item, "id");
		if (saved_id && *saved_id) {
			bfree(target->id);
			target->id = bstrdup(saved_id);
		}

		/* Load scene model if present */
		if (target->use_private_scenes) {
			obs_data_t *sm_data =
				obs_data_get_obj(item, "scene_model");
			if (sm_data) {
				target->scene_model =
					recast_config_load_scene_model(
						sm_data);
				obs_data_release(sm_data);
			}
			if (!target->scene_model)
				target->scene_model =
					recast_scene_model_create();
		}

		addCard(target);

		obs_data_release(item);
	}

	obs_data_array_release(arr);
}

void RecastDock::saveToConfig()
{
	obs_data_array_t *arr = obs_data_array_create();

	for (auto *card : cards) {
		recast_output_target_t *t = card->target();
		obs_data_t *item = obs_data_create();

		obs_data_set_string(item, "id", t->id);
		obs_data_set_string(item, "name", t->name);
		obs_data_set_string(item, "url", t->url);
		obs_data_set_string(item, "key", t->key);
		obs_data_set_string(item, "scene",
				    t->scene_name ? t->scene_name : "");
		obs_data_set_bool(item, "enabled", t->enabled);
		obs_data_set_bool(item, "autoStart", t->auto_start);
		obs_data_set_bool(item, "autoStop", t->auto_stop);
		obs_data_set_int(item, "width", t->width);
		obs_data_set_int(item, "height", t->height);
		obs_data_set_bool(item, "usePrivateScenes",
				  t->use_private_scenes);

		/* Encoding config */
		obs_data_set_bool(item, "advancedEncoder",
				  t->advanced_encoder);
		obs_data_set_string(item, "encoderId",
				    t->encoder_id ? t->encoder_id : "");
		obs_data_set_int(item, "customBitrate", t->custom_bitrate);
		obs_data_set_int(item, "audioTrack", t->audio_track);

		if (t->encoder_settings) {
			obs_data_set_obj(item, "encoderSettings",
					 t->encoder_settings);
		}

		/* Recording config */
		obs_data_set_bool(item, "recEnabled", t->rec_enabled);
		obs_data_set_string(item, "recPath",
				    t->rec_path ? t->rec_path : "");
		obs_data_set_string(item, "recFormat",
				    t->rec_format ? t->rec_format : "mkv");
		obs_data_set_int(item, "recBitrate", t->rec_bitrate);

		/* Save scene model if using private scenes */
		if (t->use_private_scenes && t->scene_model) {
			obs_data_t *sm_data =
				recast_config_save_scene_model(
					t->scene_model);
			if (sm_data) {
				obs_data_set_obj(item, "scene_model",
						 sm_data);
				obs_data_release(sm_data);
			}
		}

		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}

	recast_config_save(arr);
	obs_data_array_release(arr);
}

void RecastDock::addCard(recast_output_target_t *target)
{
	auto *card = new RecastOutputCard(target, this);
	connect(card, &RecastOutputCard::deleteRequested,
		this, &RecastDock::onDeleteOutput);
	connect(card, &RecastOutputCard::clicked,
		this, &RecastDock::onCardClicked);
	connect(card, &RecastOutputCard::autoChanged,
		this, [this](RecastOutputCard *) { saveToConfig(); });
	cards_layout->addWidget(card);
	cards.push_back(card);
	card->refreshStatus();

	/* Register hotkeys for this output */
	registerHotkeys(target);

	/* Create per-output docks if using independent scenes */
	if (target->use_private_scenes) {
		dock_manager_->createDocksForOutput(target);
	}
}

void RecastDock::removeCard(RecastOutputCard *card)
{
	auto it = std::find(cards.begin(), cards.end(), card);
	if (it == cards.end())
		return;

	/* If deleting the selected card, clear preview */
	if (card == selected_card) {
		selected_card = nullptr;
		preview->ClearScene();
		preview_label->setText(
			obs_module_text("Recast.PreviewNone"));
	}

	recast_output_target_t *t = card->target();

	/* Unregister hotkeys */
	size_t card_index = (size_t)(it - cards.begin());
	unregisterHotkeys(card_index);
	if (card_index < hotkeys_.size())
		hotkeys_.erase(hotkeys_.begin() + card_index);

	/* Destroy per-output docks first */
	if (t->use_private_scenes) {
		dock_manager_->destroyDocksForOutput(t);
	}

	cards.erase(it);
	cards_layout->removeWidget(card);
	card->deleteLater();

	recast_output_target_destroy(t);
}

void RecastDock::onCardClicked(RecastOutputCard *card)
{
	selectCard(card);
}

void RecastDock::selectCard(RecastOutputCard *card)
{
	/* Deselect previous */
	if (selected_card)
		selected_card->setSelected(false);

	selected_card = card;

	if (!card) {
		preview->ClearScene();
		preview_label->setText(
			obs_module_text("Recast.PreviewNone"));
		return;
	}

	card->setSelected(true);

	recast_output_target_t *t = card->target();

	/* If using private scenes, the preview is in the per-output dock */
	if (t->use_private_scenes) {
		preview->ClearScene();
		preview_label->setText(
			obs_module_text("Recast.PreviewInDock"));
		return;
	}

	/* Legacy: resolve the scene source */
	obs_source_t *scene = nullptr;
	if (t->scene_name && *t->scene_name) {
		scene = obs_get_source_by_name(t->scene_name);
	}
	if (!scene) {
		scene = obs_frontend_get_current_scene();
	}

	/* Resolve canvas dimensions */
	int w = t->width;
	int h = t->height;
	if (w <= 0 || h <= 0) {
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi)) {
			w = ovi.base_width;
			h = ovi.base_height;
		}
	}

	preview->SetScene(scene, w, h);
	preview_label->setText(
		QString(obs_module_text("Recast.Preview"))
			.arg(QString::fromUtf8(t->name)));

	/* SetScene takes its own ref, release ours */
	obs_source_release(scene);
}

/* ====================================================================
 * C interface -- called from plugin-main.c
 * ==================================================================== */

static RecastDock *dock_instance = nullptr;

void recast_dock_create(void)
{
	if (dock_instance)
		return;

	dock_instance = new RecastDock(
		static_cast<QWidget *>(obs_frontend_get_main_window()));

	obs_frontend_add_dock_by_id("RecastOutputDock",
				    obs_module_text("Recast.DockTitle"),
				    dock_instance);
}

void recast_dock_destroy(void)
{
	/* OBS manages the widget lifecycle, but we clear our reference */
	dock_instance = nullptr;
}
