#pragma once

#include <QAbstractListModel>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QStyledItemDelegate>

#include <vector>

extern "C" {
#include <obs.h>
}

/* ---- Single source item widget ---- */

class SourceTreeItem : public QFrame {
	Q_OBJECT

public:
	explicit SourceTreeItem(obs_sceneitem_t *item,
				QWidget *parent = nullptr);
	~SourceTreeItem();

	obs_sceneitem_t *sceneItem() const { return item_; }
	void setSelected(bool selected);
	void update();

signals:
	void visibilityToggled(obs_sceneitem_t *item, bool visible);
	void lockToggled(obs_sceneitem_t *item, bool locked);
	void renamed(obs_sceneitem_t *item, const QString &new_name);

protected:
	void mouseDoubleClickEvent(QMouseEvent *event) override;

private slots:
	void onVisibilityClicked();
	void onLockClicked();
	void onRenameFinished();
	void onRenameEscaped();

private:
	obs_sceneitem_t *item_;
	QLabel *icon_label_;
	QLabel *name_label_;
	QLineEdit *name_edit_;
	QPushButton *vis_btn_;
	QPushButton *lock_btn_;
	bool renaming_ = false;

	QIcon getSourceTypeIcon(obs_source_t *source);
	void startRename();
	void cancelRename();
};

/* ---- Source tree model ---- */

class SourceTreeModel : public QAbstractListModel {
	Q_OBJECT

public:
	explicit SourceTreeModel(QObject *parent = nullptr);

	void setScene(obs_scene_t *scene);
	obs_scene_t *scene() const { return scene_; }

	obs_sceneitem_t *itemAt(int row) const;
	int findItem(obs_sceneitem_t *item) const;

	/* QAbstractListModel */
	int rowCount(const QModelIndex &parent = QModelIndex()) const override;
	QVariant data(const QModelIndex &index,
		      int role = Qt::DisplayRole) const override;
	Qt::ItemFlags flags(const QModelIndex &index) const override;

	/* Drag and drop */
	Qt::DropActions supportedDropActions() const override;
	QStringList mimeTypes() const override;
	QMimeData *mimeData(const QModelIndexList &indexes) const override;
	bool dropMimeData(const QMimeData *data, Qt::DropAction action,
			  int row, int column,
			  const QModelIndex &parent) override;

signals:
	void orderChanged();

private:
	obs_scene_t *scene_ = nullptr;
	std::vector<obs_sceneitem_t *> items_;

	void populate();
};

/* ---- Source tree list view ---- */

class SourceTree : public QListView {
	Q_OBJECT

public:
	explicit SourceTree(QWidget *parent = nullptr);
	~SourceTree();

	void setScene(obs_scene_t *scene);

	obs_sceneitem_t *selectedItem() const;
	void selectItem(obs_sceneitem_t *item);

	void refreshItems();

signals:
	void itemSelected(obs_sceneitem_t *item);
	void sourcesModified();

protected:
	void keyPressEvent(QKeyEvent *event) override;

private slots:
	void onSelectionChanged();
	void onItemVisibilityToggled(obs_sceneitem_t *item, bool visible);
	void onItemLockToggled(obs_sceneitem_t *item, bool locked);
	void onItemRenamed(obs_sceneitem_t *item, const QString &name);
	void onOrderChanged();

private:
	SourceTreeModel *model_;

	void setupItemWidgets();
};
