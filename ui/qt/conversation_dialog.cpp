/* conversation_dialog.cpp
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "conversation_dialog.h"

#include <epan/prefs.h>
#include <epan/to_str.h>
#include <epan/dissectors/packet-tcp.h>

#include "ui/recent.h"
#include "ui/tap-tcp-stream.h"

#include "wsutil/str_util.h"

#include <ui/qt/utils/qt_ui_utils.h>
#include <ui/qt/models/timeline_delegate.h>
#include "main_application.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QPushButton>

// To do:
// - https://gitlab.com/wireshark/wireshark/-/issues/6727
//   - Wide last column?
//   + No arrows on unsorted columns
//   - Add follow stream to context menu
//   + Change "A <- B" to "B -> A"
// - Improper wildcard handling https://gitlab.com/wireshark/wireshark/-/issues/8010
// - TShark consolidation https://gitlab.com/wireshark/wireshark/-/issues/6310
// - Display filter entry?
// - Add follow, copy & graph actions to context menu.

// Bugs:
// - Slow for large numbers of items.
// - Name resolution doesn't do anything if its preference is disabled.
// - Columns don't resize correctly.
// - Closing the capture file clears conversation data.

// Fixed bugs:
// - Friendly unit displays https://gitlab.com/wireshark/wireshark/-/issues/9231
// - Misleading bps calculation https://gitlab.com/wireshark/wireshark/-/issues/8703
// - Show Absolute time in conversation tables https://gitlab.com/wireshark/wireshark/-/issues/11618
// - The value of 'Rel start' and 'Duration' in "Conversations" no need too precise https://gitlab.com/wireshark/wireshark/-/issues/12803


typedef enum {
    CONV_COLUMN_SRC_ADDR,
    CONV_COLUMN_SRC_PORT,
    CONV_COLUMN_DST_ADDR,
    CONV_COLUMN_DST_PORT,
    CONV_COLUMN_PACKETS,
    CONV_COLUMN_BYTES,
    CONV_COLUMN_PKT_AB,
    CONV_COLUMN_BYTES_AB,
    CONV_COLUMN_PKT_BA,
    CONV_COLUMN_BYTES_BA,
    CONV_COLUMN_START,
    CONV_COLUMN_DURATION,
    CONV_COLUMN_BPS_AB,
    CONV_COLUMN_BPS_BA,
    CONV_NUM_COLUMNS,
    CONV_INDEX_COLUMN = CONV_NUM_COLUMNS
} conversation_column_type_e;

static const QString table_name_ = QObject::tr("Conversation");
ConversationDialog::ConversationDialog(QWidget &parent, CaptureFile &cf, int cli_proto_id, const char *filter) :
    TrafficTableDialog(parent, cf, filter, table_name_),
    tcp_graph_requested_(false)
{
    follow_bt_ = buttonBox()->addButton(tr("Follow Stream…"), QDialogButtonBox::ActionRole);
    follow_bt_->setToolTip(tr("Follow a TCP or UDP stream."));
    connect(follow_bt_, SIGNAL(clicked()), this, SLOT(followStream()));

    graph_bt_ = buttonBox()->addButton(tr("Graph…"), QDialogButtonBox::ActionRole);
    graph_bt_->setToolTip(tr("Graph a TCP conversation."));
    connect(graph_bt_, SIGNAL(clicked()), this, SLOT(graphTcp()));

    connect(mainApp->mainWindow(), SIGNAL(displayFilterSuccess(bool)),
            this, SLOT(displayFilterSuccess(bool)));

    absoluteTimeCheckBox()->show();

    addProgressFrame(&parent);

    QList<int> conv_protos;
    for (GList *conv_tab = recent.conversation_tabs; conv_tab; conv_tab = conv_tab->next) {
        int proto_id = proto_get_id_by_short_name((const char *)conv_tab->data);
        if (proto_id > -1 && !conv_protos.contains(proto_id)) {
            conv_protos.append(proto_id);
        }
    }

    if (conv_protos.isEmpty()) {
        conv_protos = defaultProtos();
    }

    // Bring the command-line specified type to the front.
    if ((cli_proto_id > 0) && (get_conversation_by_proto_id(cli_proto_id))) {
        conv_protos.removeAll(cli_proto_id);
        conv_protos.prepend(cli_proto_id);
    }

    // QTabWidget selects the first item by default.
    foreach (int conv_proto, conv_protos) {
        addTrafficTable(get_conversation_by_proto_id(conv_proto));
    }

    fillTypeMenu(conv_protos);

    QPushButton *close_bt = buttonBox()->button(QDialogButtonBox::Close);
    if (close_bt) {
        close_bt->setDefault(true);
    }

    updateWidgets();
//    currentTabChanged();

    cap_file_.delayedRetapPackets();
}

ConversationDialog::~ConversationDialog()
{
    prefs_clear_string_list(recent.conversation_tabs);
    recent.conversation_tabs = NULL;

    ConversationTreeWidget *cur_tree = qobject_cast<ConversationTreeWidget *>(trafficTableTabWidget()->currentWidget());
    foreach (QAction *ca, traffic_type_menu_.actions()) {
        int proto_id = ca->data().value<int>();
        if (proto_id_to_tree_.contains(proto_id) && ca->isChecked()) {
            char *title = g_strdup(proto_get_protocol_short_name(find_protocol_by_id(proto_id)));
            if (proto_id_to_tree_[proto_id] == cur_tree) {
                recent.conversation_tabs = g_list_prepend(recent.conversation_tabs, title);
            } else {
                recent.conversation_tabs = g_list_append(recent.conversation_tabs, title);
            }
        }
    }
}

void ConversationDialog::captureFileClosing()
{
    // Keep the dialog around but disable any controls that depend
    // on a live capture file.
    for (int i = 0; i < trafficTableTabWidget()->count(); i++) {
        ConversationTreeWidget *cur_tree = qobject_cast<ConversationTreeWidget *>(trafficTableTabWidget()->widget(i));
        disconnect(cur_tree, SIGNAL(filterAction(QString,FilterAction::Action,FilterAction::ActionType)),
                   this, SIGNAL(filterAction(QString,FilterAction::Action,FilterAction::ActionType)));
    }
    TrafficTableDialog::captureFileClosing();
}

void ConversationDialog::captureFileClosed()
{
    displayFilterCheckBox()->setEnabled(false);
    enabledTypesPushButton()->setEnabled(false);
    follow_bt_->setEnabled(false);
    graph_bt_->setEnabled(false);
    TrafficTableDialog::captureFileClosed();
}

bool ConversationDialog::addTrafficTable(register_ct_t* table)
{
    int proto_id = get_conversation_proto_id(table);

    if (!table || proto_id_to_tree_.contains(proto_id)) {
        return false;
    }

    ConversationTreeWidget *conv_tree = new ConversationTreeWidget(this, table);

    proto_id_to_tree_[proto_id] = conv_tree;
    const char* table_name = proto_get_protocol_short_name(find_protocol_by_id(proto_id));

    trafficTableTabWidget()->addTab(conv_tree, table_name);

    conv_tree->setItemDelegateForColumn(CONV_COLUMN_START, new TimelineDelegate(conv_tree));
    conv_tree->setItemDelegateForColumn(CONV_COLUMN_DURATION, new TimelineDelegate(conv_tree));

    connect(conv_tree, SIGNAL(itemSelectionChanged()),
            this, SLOT(conversationSelectionChanged()));
    connect(conv_tree, SIGNAL(titleChanged(QWidget*,QString)),
            this, SLOT(setTabText(QWidget*,QString)));
    connect(conv_tree, SIGNAL(filterAction(QString,FilterAction::Action,FilterAction::ActionType)),
            this, SIGNAL(filterAction(QString,FilterAction::Action,FilterAction::ActionType)));
    connect(nameResolutionCheckBox(), SIGNAL(toggled(bool)),
            conv_tree, SLOT(setNameResolutionEnabled(bool)));
    connect(absoluteTimeCheckBox(), SIGNAL(toggled(bool)),
            conv_tree, SLOT(updateStartTime(bool)));


    // XXX Move to ConversationTreeWidget ctor?
    QByteArray filter_utf8;
    const char *filter = NULL;
    if (displayFilterCheckBox()->isChecked()) {
        filter = cap_file_.capFile()->dfilter;
    } else if (!filter_.isEmpty()) {
        filter_utf8 = filter_.toUtf8();
        filter = filter_utf8.constData();
    }

    conv_tree->trafficTreeHash()->user_data = conv_tree;

    registerTapListener(proto_get_protocol_filter_name(proto_id), conv_tree->trafficTreeHash(), filter, 0,
                        ConversationTreeWidget::tapReset,
                        get_conversation_packet_func(table),
                        ConversationTreeWidget::tapDraw);

    return true;
}

void ConversationDialog::followStream()
{
    if (file_closed_) {
        return;
    }

    conv_item_t *conv_item = currentConversation();
    if (!conv_item) {
        return;
    }

    follow_type_t ftype;
    switch (conv_item->etype) {
    case ENDPOINT_TCP:
        ftype = FOLLOW_TCP;
        break;
    case ENDPOINT_UDP:
        ftype = FOLLOW_UDP;
        break;
    default:
        return;
    }

    // Will set the display filter too.
    // TCP and UDP do not have a "sub-stream", so set a dummy value.
    emit openFollowStreamDialog(ftype, conv_item->conv_id, 0);
}

void ConversationDialog::graphTcp()
{
    if (file_closed_) {
        return;
    }

    conv_item_t *conv_item = currentConversation();
    if (!conv_item) {
        return;
    }

    // XXX The GTK+ code opens the TCP Stream dialog. We might want
    // to open the I/O Graphs dialog instead.
    QString filter;
    if (conv_item->etype == ENDPOINT_TCP) {
        filter = QString("tcp.stream eq %1").arg(conv_item->conv_id);
    } else {
        return;
    }

    tcp_graph_requested_ = true;
    // Apply the filter for this conversation. When the filter is active, we
    // can draw the TCP graph.
    emit filterAction(filter, FilterAction::ActionApply, FilterAction::ActionTypePlain);
}

void ConversationDialog::currentTabChanged()
{
    bool copy_enable = trafficTableTabWidget()->currentWidget() ? true : false;

    copy_bt_->setEnabled(copy_enable);

    conversationSelectionChanged();
    TrafficTableDialog::currentTabChanged();
}

void ConversationDialog::conversationSelectionChanged()
{
    bool follow_enable = false, graph_enable = false;
    conv_item_t *conv_item = currentConversation();

    if (!file_closed_ && conv_item) {
        switch (conv_item->etype) {
        case ENDPOINT_TCP:
            graph_enable = true;
            // Fall through
        case ENDPOINT_UDP:
            follow_enable = true;
            break;
        default:
            break;
        }
    }
    follow_bt_->setEnabled(follow_enable);
    graph_bt_->setEnabled(graph_enable);
}

void ConversationDialog::on_displayFilterCheckBox_toggled(bool checked)
{
    if (file_closed_) {
        return;
    }

    QByteArray filter_utf8;
    const char *filter = NULL;
    if (checked) {
        filter = cap_file_.capFile()->dfilter;
    } else if (!filter_.isEmpty()) {
        filter_utf8 = filter_.toUtf8();
        filter = filter_utf8.constData();
    }

    for (int i = 0; i < trafficTableTabWidget()->count(); i++) {
        set_tap_dfilter(trafficTableTabWidget()->widget(i), filter);
    }

    cap_file_.retapPackets();
}

void ConversationDialog::on_buttonBox_helpRequested()
{
    mainApp->helpTopicAction(HELP_STATS_CONVERSATIONS_DIALOG);
}

void ConversationDialog::displayFilterSuccess(bool success)
{
    if (tcp_graph_requested_) {
        if (success) {
            // The display filter was applied successfully, i.e. the current
            // packet is now part of our selected tcp conversation.
            openTcpStreamGraph(GRAPH_TSEQ_TCPTRACE);
        }
        tcp_graph_requested_ = false;
    }
}

void init_conversation_table(struct register_ct* ct, const char *filter)
{
    mainApp->emitStatCommandSignal("Conversations", filter, GINT_TO_POINTER(get_conversation_proto_id(ct)));
}


// ConversationTreeWidgetItem
// TrafficTableTreeWidgetItem / QTreeWidgetItem subclass that allows sorting

// Minimum bandwidth calculation duration
// https://gitlab.com/wireshark/wireshark/-/issues/8703
static const double min_bw_calc_duration_ = 5 / 1000.0; // seconds
static const char *bps_na_ = UTF8_EM_DASH;

class ConversationTreeWidgetItem : public TrafficTableTreeWidgetItem
{
public:
    ConversationTreeWidgetItem(GArray *conv_array, guint conv_idx, bool *resolve_names_ptr) :
        TrafficTableTreeWidgetItem(NULL),
        conv_array_(conv_array),
        conv_idx_(conv_idx),
        resolve_names_ptr_(resolve_names_ptr)
    {
        QString timeline_tt = QObject::tr("Bars show the relative timeline for each conversation.");
        setToolTip(CONV_COLUMN_START, timeline_tt);
        setToolTip(CONV_COLUMN_DURATION, timeline_tt);
    }

    conv_item_t *convItem() {
        return &g_array_index(conv_array_, conv_item_t, conv_idx_);
    }

    virtual QVariant data(int column, int role) const {
        switch (role) {
        case Qt::DisplayRole:
        {
            // Column text cooked representation.
            conv_item_t *conv_item = &g_array_index(conv_array_, conv_item_t, conv_idx_);

            bool resolve_names = false;
            if (resolve_names_ptr_ && *resolve_names_ptr_) resolve_names = true;
            double duration = nstime_to_sec(&conv_item->stop_time) - nstime_to_sec(&conv_item->start_time);
            QString bps_ab = bps_na_, bps_ba = bps_na_;

            switch (column) {
            case CONV_COLUMN_PACKETS:
                return QString("%L1").arg(conv_item->tx_frames + conv_item->rx_frames);
            case CONV_COLUMN_BYTES:
                return gchar_free_to_qstring(format_size(conv_item->tx_bytes + conv_item->rx_bytes, FORMAT_SIZE_UNIT_NONE, FORMAT_SIZE_PREFIX_SI));
            case CONV_COLUMN_PKT_AB:
                return QString("%L1").arg(conv_item->tx_frames);
            case CONV_COLUMN_BYTES_AB:
                return gchar_free_to_qstring(format_size(conv_item->tx_bytes, FORMAT_SIZE_UNIT_NONE, FORMAT_SIZE_PREFIX_SI));
            case CONV_COLUMN_PKT_BA:
                return QString("%L1").arg(conv_item->rx_frames);
            case CONV_COLUMN_BYTES_BA:
                return gchar_free_to_qstring(format_size(conv_item->rx_bytes, FORMAT_SIZE_UNIT_NONE, FORMAT_SIZE_PREFIX_SI));
            case CONV_COLUMN_START:
            {
                bool use_ns = treeWidget()->window()->property("nanosecond_precision").toBool();
                int width = use_ns ? 9 : 6;

                if (treeWidget()->window()->property("absolute_start_time").toBool()) {
                    nstime_t *abs_time = &conv_item->start_abs_time;
                    QDateTime abs_dt = QDateTime::fromMSecsSinceEpoch(nstime_to_msec(abs_time));
                    return QString("%1.%2")
                            // Mimic column-utils:set_abs_time as best we can
                            .arg(abs_dt.toString("hh:mm:ss"))
                            .arg(use_ns ? abs_time->nsecs : abs_time->nsecs / 1000, width, 10, QChar('0'));
                }

                return QString::number(nstime_to_sec(&conv_item->start_time), 'f', width);
            }
            case CONV_COLUMN_DURATION:
            {
                // The GTK+ UI uses 9 digit precision for the start time and 4 for the duration.
                // Do the same here and above for non-nanosecond precision and add a couple
                // of digits for nanosecond precision.
                bool use_ns = treeWidget()->window()->property("nanosecond_precision").toBool();
                int width = use_ns ? 6 : 4;

                return QString::number(duration, 'f', width);
            }
            case CONV_COLUMN_BPS_AB:
                if (duration > min_bw_calc_duration_) {
                    bps_ab = gchar_free_to_qstring(format_size((gint64) conv_item->tx_bytes * 8 / duration, FORMAT_SIZE_UNIT_NONE, FORMAT_SIZE_PREFIX_SI));
                }
                return bps_ab;
            case CONV_COLUMN_BPS_BA:
                if (duration > min_bw_calc_duration_) {
                    bps_ba = gchar_free_to_qstring(format_size((gint64) conv_item->rx_bytes * 8 / duration, FORMAT_SIZE_UNIT_NONE, FORMAT_SIZE_PREFIX_SI));
                }
                return bps_ba;
            default:
                return colData(column, resolve_names).toString();
            }
            break;
        }
        case Qt::UserRole:
        {
            if (column != CONV_COLUMN_START && column != CONV_COLUMN_DURATION) break;

            ConversationTreeWidget *ctw = qobject_cast<ConversationTreeWidget *>(treeWidget());
            if (!ctw) break;

            conv_item_t *conv_item = &g_array_index(conv_array_, conv_item_t, conv_idx_);
            double start_time = nstime_to_sec(&conv_item->start_time);
            double stop_time = nstime_to_sec(&conv_item->stop_time);

            double span_s = ctw->maxRelStopTime() - ctw->minRelStartTime();
            if (span_s <= 0) break;
            int start_px = ctw->columnWidth(CONV_COLUMN_START);
            int column_px = start_px + ctw->columnWidth(CONV_COLUMN_DURATION);

            struct timeline_span span_px;
            span_px.start = ((start_time - ctw->minRelStartTime()) * column_px) / span_s;
            span_px.width = ((stop_time - start_time) * column_px) / span_s;

            if (column == CONV_COLUMN_DURATION) {
                span_px.start -= start_px;
            }
            return QVariant::fromValue(span_px);

            break;
        }
        default:
            break;
        }
        return QTreeWidgetItem::data(column, role);
    }

    // Column text raw representation.
    // Return a QString, qulonglong, double, or invalid QVariant representing the raw column data.
    QVariant colData(int col, bool resolve_names) const {
        conv_item_t *conv_item = &g_array_index(conv_array_, conv_item_t, conv_idx_);

        double duration = nstime_to_sec(&conv_item->stop_time) - nstime_to_sec(&conv_item->start_time);
        double bps_ab = 0, bps_ba = 0;
        if (duration > min_bw_calc_duration_) {
            bps_ab = conv_item->tx_bytes * 8 / duration;
            bps_ba = conv_item->rx_bytes * 8 / duration;
        }

        switch (col) {
        case CONV_COLUMN_SRC_ADDR:
            {
            char* addr_str = get_conversation_address(NULL, &conv_item->src_address, resolve_names);
            QString q_addr_str(addr_str);
            wmem_free(NULL, addr_str);
            return q_addr_str;
            }
        case CONV_COLUMN_SRC_PORT:
            if (resolve_names) {
                char* port_str = get_conversation_port(NULL, conv_item->src_port, conv_item->etype, resolve_names);
                QString q_port_str(port_str);
                wmem_free(NULL, port_str);
                return q_port_str;
            } else {
                return quint32(conv_item->src_port);
            }
        case CONV_COLUMN_DST_ADDR:
            {
            char* addr_str = get_conversation_address(NULL, &conv_item->dst_address, resolve_names);
            QString q_addr_str(addr_str);
            wmem_free(NULL, addr_str);
            return q_addr_str;
            }
        case CONV_COLUMN_DST_PORT:
            if (resolve_names) {
                char* port_str = get_conversation_port(NULL, conv_item->dst_port, conv_item->etype, resolve_names);
                QString q_port_str(port_str);
                wmem_free(NULL, port_str);
                return q_port_str;
            } else {
                return quint32(conv_item->dst_port);
            }
        case CONV_COLUMN_PACKETS:
            return quint64(conv_item->tx_frames + conv_item->rx_frames);
        case CONV_COLUMN_BYTES:
            return quint64(conv_item->tx_bytes + conv_item->rx_bytes);
        case CONV_COLUMN_PKT_AB:
            return quint64(conv_item->tx_frames);
        case CONV_COLUMN_BYTES_AB:
            return quint64(conv_item->tx_bytes);
        case CONV_COLUMN_PKT_BA:
            return quint64(conv_item->rx_frames);
        case CONV_COLUMN_BYTES_BA:
            return quint64(conv_item->rx_bytes);
        case CONV_COLUMN_START:
            return nstime_to_sec(&conv_item->start_time);
        case CONV_COLUMN_DURATION:
            return duration;
        case CONV_COLUMN_BPS_AB:
            return bps_ab;
        case CONV_COLUMN_BPS_BA:
            return bps_ba;
        default:
            return QVariant();
        }
    }

    bool operator< (const QTreeWidgetItem &other) const
    {
        const ConversationTreeWidgetItem *other_row = static_cast<const ConversationTreeWidgetItem *>(&other);
        conv_item_t *conv_item = &g_array_index(conv_array_, conv_item_t, conv_idx_);
        conv_item_t *other_item = &g_array_index(other_row->conv_array_, conv_item_t, other_row->conv_idx_);

        bool resolve_names = false;
        if (resolve_names_ptr_ && *resolve_names_ptr_) resolve_names = true;

        int sort_col = treeWidget()->sortColumn();
        double conv_duration = nstime_to_sec(&conv_item->stop_time) - nstime_to_sec(&conv_item->start_time);
        double other_duration = nstime_to_sec(&other_item->stop_time) - nstime_to_sec(&other_item->start_time);

        switch(sort_col) {
        case CONV_COLUMN_SRC_ADDR:
            if (resolve_names) {
                char* addr_str = address_to_display(NULL, &conv_item->src_address);
                char* otheraddr_str = address_to_display(NULL, &other_item->src_address);
                bool ret;

                ret = g_ascii_strcasecmp(addr_str, otheraddr_str) < 0 ? true : false;
                wmem_free(NULL, otheraddr_str);
                wmem_free(NULL, addr_str);
                return ret;
	    } else {
                return cmp_address(&conv_item->src_address, &other_item->src_address) < 0 ? true : false;
            }
        case CONV_COLUMN_SRC_PORT:
            return conv_item->src_port < other_item->src_port;
        case CONV_COLUMN_DST_ADDR:
            if (resolve_names) {
                char* addr_str = address_to_display(NULL, &conv_item->dst_address);
                char* otheraddr_str = address_to_display(NULL, &other_item->dst_address);
                bool ret;

                ret = g_ascii_strcasecmp(addr_str, otheraddr_str) < 0 ? true : false;
                wmem_free(NULL, otheraddr_str);
                wmem_free(NULL, addr_str);
                return ret;
	    } else {
                return cmp_address(&conv_item->dst_address, &other_item->dst_address) < 0 ? true : false;
            }
        case CONV_COLUMN_DST_PORT:
            return conv_item->dst_port < other_item->dst_port;
        case CONV_COLUMN_PACKETS:
            return (conv_item->tx_frames + conv_item->rx_frames) < (other_item->tx_frames + other_item->rx_frames);
        case CONV_COLUMN_BYTES:
            return (conv_item->tx_bytes + conv_item->rx_bytes) < (other_item->tx_bytes + other_item->rx_bytes);
        case CONV_COLUMN_PKT_AB:
            return conv_item->tx_frames < other_item->tx_frames;
        case CONV_COLUMN_BYTES_AB:
            return conv_item->tx_bytes < other_item->tx_bytes;
        case CONV_COLUMN_PKT_BA:
            return conv_item->rx_frames < other_item->rx_frames;
        case CONV_COLUMN_BYTES_BA:
            return conv_item->rx_bytes < other_item->rx_bytes;
        case CONV_COLUMN_START:
            return nstime_to_sec(&conv_item->start_time) < nstime_to_sec(&other_item->start_time);
        case CONV_COLUMN_DURATION:
            return conv_duration < other_duration;
        case CONV_COLUMN_BPS_AB:
            return conv_item->tx_bytes / conv_duration < other_item->tx_bytes / other_duration;
        case CONV_COLUMN_BPS_BA:
            return conv_item->rx_bytes / conv_duration < other_item->rx_bytes / other_duration;
        default:
            return false;
        }
    }
private:
    GArray *conv_array_;
    guint conv_idx_;
    bool *resolve_names_ptr_;
};

conv_item_t *ConversationDialog::currentConversation()
{
    ConversationTreeWidget *cur_tree = qobject_cast<ConversationTreeWidget *>(trafficTableTabWidget()->currentWidget());

    if (!cur_tree || cur_tree->selectedItems().count() < 1) {
        return NULL;
    }

    ConversationTreeWidgetItem *ctwi = dynamic_cast<ConversationTreeWidgetItem *>(cur_tree->selectedItems()[0]);
    return (ctwi ? ctwi->convItem() : NULL);
}

// ConversationTreeWidget
// TrafficTableTreeWidget / QTreeWidget subclass that allows tapping

ConversationTreeWidget::ConversationTreeWidget(QWidget *parent, register_ct_t* table) :
    TrafficTableTreeWidget(parent, table),
    min_rel_start_time_(0),
    max_rel_stop_time_(0)
{
    setColumnCount(CONV_NUM_COLUMNS);
    setUniformRowHeights(true);

    for (int i = 0; i < CONV_NUM_COLUMNS; i++) {
        headerItem()->setText(i, columnTitle(i));
    }

    if (get_conversation_hide_ports(table_)) {
        hideColumn(CONV_COLUMN_SRC_PORT);
        hideColumn(CONV_COLUMN_DST_PORT);
    } else if (!strcmp(proto_get_protocol_filter_name(get_conversation_proto_id(table_)), "ncp")) {
        headerItem()->setText(CONV_COLUMN_SRC_PORT, tr("Connection A"));
        headerItem()->setText(CONV_COLUMN_DST_PORT, tr("Connection B"));
    }

    int one_en = fontMetrics().height() / 2;
    for (int i = 0; i < CONV_NUM_COLUMNS; i++) {
        switch (i) {
        case CONV_COLUMN_SRC_ADDR:
        case CONV_COLUMN_DST_ADDR:
            setColumnWidth(i, one_en * (int) strlen("000.000.000.000"));
            break;
        case CONV_COLUMN_SRC_PORT:
        case CONV_COLUMN_DST_PORT:
            setColumnWidth(i, one_en * (int) strlen("000000"));
            break;
        case CONV_COLUMN_PACKETS:
        case CONV_COLUMN_PKT_AB:
        case CONV_COLUMN_PKT_BA:
            setColumnWidth(i, one_en * (int) strlen("00,000"));
            break;
        case CONV_COLUMN_BYTES:
        case CONV_COLUMN_BYTES_AB:
        case CONV_COLUMN_BYTES_BA:
            setColumnWidth(i, one_en * (int) strlen("000,000"));
            break;
        case CONV_COLUMN_START:
            setColumnWidth(i, one_en * (int) strlen("00.000"));
            break;
        case CONV_COLUMN_DURATION:
            setColumnWidth(i, one_en * (int) strlen("00.000000"));
            break;
        case CONV_COLUMN_BPS_AB:
        case CONV_COLUMN_BPS_BA:
            setColumnWidth(i, one_en * (int) strlen("000 k"));
            break;
        default:
            setColumnWidth(i, one_en * 5);
        }
    }

    QMenu *submenu;

    initDirectionMap();

    FilterAction::Action cur_action = FilterAction::ActionApply;
    submenu = ctx_menu_.addMenu(FilterAction::actionName(cur_action));
    foreach (FilterAction::ActionType at, FilterAction::actionTypes()) {
        QMenu *subsubmenu = submenu->addMenu(FilterAction::actionTypeName(at));
        foreach (FilterAction::ActionDirection ad, FilterAction::actionDirections()) {
            FilterAction *fa = new FilterAction(subsubmenu, cur_action, at, ad);
            subsubmenu->addAction(fa);
            connect(fa, SIGNAL(triggered()), this, SLOT(filterActionTriggered()));
        }
    }

    cur_action = FilterAction::ActionPrepare;
    submenu = ctx_menu_.addMenu(FilterAction::actionName(cur_action));
    foreach (FilterAction::ActionType at, FilterAction::actionTypes()) {
        QMenu *subsubmenu = submenu->addMenu(FilterAction::actionTypeName(at));
        foreach (FilterAction::ActionDirection ad, FilterAction::actionDirections()) {
            FilterAction *fa = new FilterAction(subsubmenu, cur_action, at, ad);
            subsubmenu->addAction(fa);
            connect(fa, SIGNAL(triggered()), this, SLOT(filterActionTriggered()));
        }
    }

    cur_action = FilterAction::ActionFind;
    submenu = ctx_menu_.addMenu(FilterAction::actionName(cur_action));
    foreach (FilterAction::ActionDirection ad, FilterAction::actionDirections()) {
        FilterAction *fa = new FilterAction(submenu, cur_action, FilterAction::ActionTypePlain, ad);
        submenu->addAction(fa);
        connect(fa, SIGNAL(triggered()), this, SLOT(filterActionTriggered()));
    }

    cur_action = FilterAction::ActionColorize;
    submenu = ctx_menu_.addMenu(FilterAction::actionName(cur_action));
    foreach (FilterAction::ActionDirection ad, FilterAction::actionDirections()) {
        FilterAction *fa = new FilterAction(submenu, cur_action, FilterAction::ActionTypePlain, ad);
        submenu->addAction(fa);
        connect(fa, SIGNAL(triggered()), this, SLOT(filterActionTriggered()));
    }

    ctx_menu_.addSeparator();
    QAction * act = ctx_menu_.addAction(tr("Resize all columns to content"));
    connect(act, &QAction::triggered, [this]() {
        for (int col = 0; col < this->columnCount(); col++)
            this->resizeColumnToContents(col);
    });

    updateItems();
}

ConversationTreeWidget::~ConversationTreeWidget() {
    reset_conversation_table_data(&hash_);
}

QString ConversationTreeWidget::columnTitle(int col)
{
    switch (col) {
        case 0:
            return tr("Address A"); break;
        case 1:
            return tr("Port A"); break;
        case 2:
            return tr("Address B"); break;
        case 3:
            return tr("Port B"); break;
        case 4:
            return tr("Packets"); break;
        case 5:
            return tr("Bytes"); break;
        case 6:
            return tr("Packets A " UTF8_RIGHTWARDS_ARROW " B"); break;
        case 7:
            return tr("Bytes A " UTF8_RIGHTWARDS_ARROW " B"); break;
        case 8:
            return tr("Packets B " UTF8_RIGHTWARDS_ARROW " A"); break;
        case 9:
            return tr("Packets B " UTF8_RIGHTWARDS_ARROW " A"); break;
        case 10:
            return tr("Rel Start"); break;
        case 11:
            return tr("Duration"); break;
        case 12:
            return tr("Bits/s A " UTF8_RIGHTWARDS_ARROW " B"); break;
        case 13:
            return tr("Bits/s B " UTF8_RIGHTWARDS_ARROW " A"); break;
    }

    return QString();
}

// Callbacks for register_tap_listener
void ConversationTreeWidget::tapReset(void *conv_hash_ptr)
{
    conv_hash_t *hash = (conv_hash_t*)conv_hash_ptr;
    ConversationTreeWidget *conv_tree = qobject_cast<ConversationTreeWidget *>((ConversationTreeWidget *)hash->user_data);
    if (!conv_tree) return;

    conv_tree->clear();
    reset_conversation_table_data(&conv_tree->hash_);
    conv_tree->min_rel_start_time_ = 0;
    conv_tree->max_rel_stop_time_ = 0;
}

void ConversationTreeWidget::tapDraw(void *conv_hash_ptr)
{
    conv_hash_t *hash = (conv_hash_t*)conv_hash_ptr;
    ConversationTreeWidget *conv_tree = qobject_cast<ConversationTreeWidget *>((ConversationTreeWidget *)hash->user_data);
    if (!conv_tree) return;

    conv_tree->updateItems();
}

void ConversationTreeWidget::updateStartTime(bool absolute)
{
    headerItem()->setText(CONV_COLUMN_START, absolute
                          ? tr("Abs Start")
                          : columnTitle(CONV_COLUMN_START));

    dataChanged(QModelIndex(), QModelIndex());

    if (topLevelItemCount() > 0) {
        resizeColumnToContents(CONV_COLUMN_START);
    }
}

QMap<FilterAction::ActionDirection, conv_direction_e> fad_to_cd_;

void ConversationTreeWidget::initDirectionMap()
{
    if (fad_to_cd_.size() > 0) {
        return;
    }

    fad_to_cd_[FilterAction::ActionDirectionAToFromB] = CONV_DIR_A_TO_FROM_B;
    fad_to_cd_[FilterAction::ActionDirectionAToB] = CONV_DIR_A_TO_B;
    fad_to_cd_[FilterAction::ActionDirectionAFromB] = CONV_DIR_A_FROM_B;
    fad_to_cd_[FilterAction::ActionDirectionAToFromAny] = CONV_DIR_A_TO_FROM_ANY;
    fad_to_cd_[FilterAction::ActionDirectionAToAny] = CONV_DIR_A_TO_ANY;
    fad_to_cd_[FilterAction::ActionDirectionAFromAny] = CONV_DIR_A_FROM_ANY;
    fad_to_cd_[FilterAction::ActionDirectionAnyToFromB] = CONV_DIR_ANY_TO_FROM_B;
    fad_to_cd_[FilterAction::ActionDirectionAnyToB] = CONV_DIR_ANY_TO_B;
    fad_to_cd_[FilterAction::ActionDirectionAnyFromB] = CONV_DIR_ANY_FROM_B;
}

void ConversationTreeWidget::updateItems() {
    bool resize = topLevelItemCount() < resizeThreshold();
    title_ = proto_get_protocol_short_name(find_protocol_by_id(get_conversation_proto_id(table_)));

    if (hash_.conv_array && hash_.conv_array->len > 0) {
        title_.append(QString(" %1 %2").arg(UTF8_MIDDLE_DOT).arg(hash_.conv_array->len));
    }
    emit titleChanged(this, title_);

    if (!hash_.conv_array) {
        return;
    }

    setSortingEnabled(false);

    QList<QTreeWidgetItem *>new_items;
    for (int i = topLevelItemCount(); i < (int) hash_.conv_array->len; i++) {
        ConversationTreeWidgetItem *ctwi = new ConversationTreeWidgetItem(hash_.conv_array, i, &resolve_names_);
        new_items << ctwi;

        if (i == 0) {
            min_rel_start_time_ = nstime_to_sec(&(ctwi->convItem()->start_time));
            max_rel_stop_time_ = nstime_to_sec(&(ctwi->convItem()->stop_time));
        }

        for (int col = 0; col < columnCount(); col++) {
            switch (col) {
            case CONV_COLUMN_SRC_ADDR:
            case CONV_COLUMN_DST_ADDR:
            break;
            default:
                ctwi->setTextAlignment(col, Qt::AlignRight);
                break;
            }
        }
    }
    addTopLevelItems(new_items);

    for (int i = 0; i < topLevelItemCount(); i++) {
        ConversationTreeWidgetItem *ctwi = dynamic_cast<ConversationTreeWidgetItem *>(topLevelItem(i));

        if (ctwi) {
            double item_rel_start = nstime_to_sec(&(ctwi->convItem()->start_time));
            if (item_rel_start < min_rel_start_time_) {
                min_rel_start_time_ = item_rel_start;
            }

            double item_rel_stop = nstime_to_sec(&(ctwi->convItem()->stop_time));
            if (item_rel_stop > max_rel_stop_time_) {
                max_rel_stop_time_ = item_rel_stop;
            }
        }
    }

    setSortingEnabled(true);

    if (resize) {
        for (int col = 0; col < columnCount(); col++) {
            resizeColumnToContents(col);
        }
    }
}

void ConversationTreeWidget::filterActionTriggered()
{
    ConversationTreeWidgetItem *ctwi = static_cast<ConversationTreeWidgetItem *>(currentItem());
    FilterAction *fa = qobject_cast<FilterAction *>(QObject::sender());

    if (!fa || !ctwi) {
        return;
    }

    conv_item_t *conv_item = ctwi->convItem();
    if (!conv_item) {
        return;
    }

    char* tmp_str = get_conversation_filter(conv_item, fad_to_cd_[fa->actionDirection()]);
    QString filter(tmp_str);

    g_free(tmp_str);
    emit filterAction(filter, fa->action(), fa->actionType());
}
