#pragma once
#include <QWidget>
#include <QAbstractTableModel>

class QTableView;
class QSpinBox;
class QLabel;
class QPushButton;
class QStyledItemDelegate;

class OrderListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit OrderListModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &p = {}) const override;
    int columnCount(const QModelIndex &p = {}) const override;
    QVariant data(const QModelIndex &i, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;
    bool setData(const QModelIndex &i, const QVariant &v, int role) override;
    Qt::ItemFlags flags(const QModelIndex &i) const override;
    void refresh();
};

class OrderView : public QWidget {
    Q_OBJECT
public:
    explicit OrderView(QWidget *parent = nullptr);
    void refresh();

signals:
    void edited();

private slots:
    void onSubtuneChanged(int v);
    void onSelectionChanged();
    void insertRow();
    void deleteRow();
    void insertRowAllChannels();
    void deleteRowAllChannels();
    void insertTransposeUp();
    void insertTransposeDown();
    void insertRepeat();
    void insertRst();
    void gotoPattern();
    void swap12();
    void swap13();
    void swap23();

private:
    QTableView *table_ = nullptr;
    OrderListModel *model_ = nullptr;
    QSpinBox *subtuneSpin_ = nullptr;
    QLabel *patternPreview_ = nullptr;
    bool updating_ = false;

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void writeAtCursor(unsigned char v);
    void syncCursorFromGlobal();
    void syncGlobalFromCursor();
    void updatePreview();
};
