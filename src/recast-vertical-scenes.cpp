/*
 * recast-vertical-scenes.cpp -- Always-present vertical scenes dock.
 *
 * Provides a list of private scenes for the vertical canvas,
 * with add/remove/rename/link controls matching native OBS look.
 */

#include "recast-vertical-scenes.h"
#include "recast-vertical.h"

#include <QAction>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QVBoxLayout>

extern "C" {
#include <obs-module.h>
#include <obs-frontend-api.h>
}

RecastVerticalScenesDock::RecastVerticalScenesDock(QWidget *parent)
	: QDockWidget(obs_module_text("Recast.Vertical.Scenes"), parent)
{
	setObjectName("RecastVerticalScenesDock");
	setFeatures(QDockWidget::DockWidgetMovable |
		    QDockWidget::DockWidgetFloatable |
		    QDockWidget::DockWidgetClosable);
	setTitleBarWidget(new QWidget());

	auto *container = new QWidget;
	auto *layout = new QVBoxLayout(container);
	layout->setContentsMargins(4, 4, 4, 4);

	/* Scene list */
	list_ = new QListWidget;
	list_->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(list_, &QListWidget::currentRowChanged,
		this, &RecastVerticalScenesDock::onSceneSelected);
	connect(list_, &QListWidget::customContextMenuRequested,
		this, &RecastVerticalScenesDock::onContextMenu);
	layout->addWidget(list_);

	/* Toolbar */
	auto *toolbar = new QHBoxLayout;

	add_btn_ = new QPushButton("+");
	add_btn_->setFixedSize(28, 28);
	add_btn_->setToolTip(obs_module_text("Recast.Scenes.Add"));
	add_btn_->setStyleSheet(
		"QPushButton { font-size: 16px; font-weight: bold; "
		"border: 1px solid #444; border-radius: 4px; "
		"background: #001c3f; color: #fff; }"
		"QPushButton:hover { background: #002a5f; }");
	connect(add_btn_, &QPushButton::clicked,
		this, &RecastVerticalScenesDock::onAddScene);
	toolbar->addWidget(add_btn_);

	remove_btn_ = new QPushButton(QString::fromUtf8("\xe2\x80\x93")); /* en-dash */
	remove_btn_->setFixedSize(28, 28);
	remove_btn_->setToolTip(obs_module_text("Recast.Scenes.Remove"));
	remove_btn_->setStyleSheet(
		"QPushButton { font-size: 16px; font-weight: bold; "
		"border: 1px solid #444; border-radius: 4px; "
		"background: #910000; color: #fff; }"
		"QPushButton:hover { background: #b10000; }");
	connect(remove_btn_, &QPushButton::clicked,
		this, &RecastVerticalScenesDock::onRemoveScene);
	toolbar->addWidget(remove_btn_);

	toolbar->addStretch();

	layout->addLayout(toolbar);

	setWidget(container);

	refresh();
}

RecastVerticalScenesDock::~RecastVerticalScenesDock() {}

void RecastVerticalScenesDock::refresh()
{
	list_->blockSignals(true);
	list_->clear();

	RecastVertical *v = RecastVertical::instance();
	int count = v->sceneCount();

	for (int i = 0; i < count; i++) {
		const char *name = v->sceneName(i);
		list_->addItem(QString::fromUtf8(name ? name : "(unnamed)"));
	}

	int active = v->activeSceneIndex();
	if (active >= 0 && active < count)
		list_->setCurrentRow(active);

	list_->blockSignals(false);
}

void RecastVerticalScenesDock::onAddScene()
{
	bool ok;
	QString name = QInputDialog::getText(
		this, obs_module_text("Recast.Scenes.Add"),
		obs_module_text("Recast.Scenes.EnterName"),
		QLineEdit::Normal, QString(), &ok);

	if (!ok || name.trimmed().isEmpty())
		return;

	RecastVertical *v = RecastVertical::instance();
	v->addScene(name.trimmed().toUtf8().constData());

	refresh();
	emit scenesModified();
}

void RecastVerticalScenesDock::onRemoveScene()
{
	int row = list_->currentRow();
	if (row < 0)
		return;

	auto answer = QMessageBox::question(
		this, obs_module_text("Recast.Confirm"),
		QString("%1 '%2'?")
			.arg(obs_module_text("Recast.Scenes.ConfirmRemove"))
			.arg(list_->item(row)->text()));

	if (answer != QMessageBox::Yes)
		return;

	RecastVertical *v = RecastVertical::instance();
	v->removeScene(row);

	refresh();
	emit scenesModified();
}

void RecastVerticalScenesDock::onSceneSelected(int row)
{
	if (row < 0)
		return;

	RecastVertical *v = RecastVertical::instance();
	v->setActiveScene(row);

	emit sceneActivated(row);
}

void RecastVerticalScenesDock::onContextMenu(const QPoint &pos)
{
	QMenu menu;

	int row = list_->currentRow();

	/* Add Scene -- always available */
	QAction *add_action =
		menu.addAction(obs_module_text("Recast.Scenes.Add"));
	connect(add_action, &QAction::triggered, this,
		&RecastVerticalScenesDock::onAddScene);

	if (row >= 0) {
		menu.addSeparator();

		/* Rename */
		QAction *rename_action =
			menu.addAction(obs_module_text("Recast.Scenes.Rename"));
		connect(rename_action, &QAction::triggered, this,
			[this, row]() {
				bool ok;
				QString new_name = QInputDialog::getText(
					this,
					obs_module_text("Recast.Scenes.Rename"),
					obs_module_text("Recast.Scenes.EnterName"),
					QLineEdit::Normal,
					list_->item(row)->text(), &ok);
				if (!ok || new_name.trimmed().isEmpty())
					return;
				RecastVertical *v = RecastVertical::instance();
				v->renameScene(
					row,
					new_name.trimmed().toUtf8().constData());
				refresh();
				emit scenesModified();
			});

		/* Link to Main Scene */
		QMenu *link_menu = menu.addMenu(
			obs_module_text("Recast.Vertical.LinkToMain"));

		RecastVertical *v = RecastVertical::instance();
		const char *current_link = v->sceneLinkedMain(row);

		/* Unlink option */
		if (current_link) {
			QAction *unlink = link_menu->addAction(
				QString("Unlink (currently: %1)")
					.arg(QString::fromUtf8(current_link)));
			connect(unlink, &QAction::triggered, this,
				[this, row]() {
					RecastVertical *v =
						RecastVertical::instance();
					v->linkScene(row, nullptr);
					emit scenesModified();
				});
			link_menu->addSeparator();
		}

		/* List all main scenes */
		char **main_scenes = obs_frontend_get_scene_names();
		if (main_scenes) {
			for (char **s = main_scenes; *s; s++) {
				QString scene_name =
					QString::fromUtf8(*s);
				QAction *link_action =
					link_menu->addAction(scene_name);
				connect(link_action,
					&QAction::triggered, this,
					[this, row, scene_name]() {
						RecastVertical *v =
							RecastVertical::instance();
						v->linkScene(
							row,
							scene_name.toUtf8()
								.constData());
						emit scenesModified();
					});
			}
			bfree(main_scenes);
		}

		menu.addSeparator();

		/* Remove */
		QAction *remove_action = menu.addAction(
			obs_module_text("Recast.Scenes.Remove"));
		connect(remove_action, &QAction::triggered, this,
			&RecastVerticalScenesDock::onRemoveScene);
	}

	menu.exec(list_->mapToGlobal(pos));
}
