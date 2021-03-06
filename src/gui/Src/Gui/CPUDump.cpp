#include "CPUDump.h"
#include <QMessageBox>
#include <QClipboard>
#include <QFileDialog>
#include <QToolTip>
#include "Configuration.h"
#include "Bridge.h"
#include "LineEditDialog.h"
#include "HexEditDialog.h"
#include "YaraRuleSelectionDialog.h"
#include "DataCopyDialog.h"
#include "EntropyDialog.h"
#include "CPUMultiDump.h"
#include "GotoDialog.h"
#include "CPUDisassembly.h"
#include "WordEditDialog.h"
#include "CodepageSelectionDialog.h"
#include "MiscUtil.h"

CPUDump::CPUDump(CPUDisassembly* disas, CPUMultiDump* multiDump, QWidget* parent) : HexDump(parent)
{
    mDisas = disas;
    mMultiDump = multiDump;

    setView((ViewEnum_t)ConfigUint("HexDump", "DefaultView"));

    connect(this, SIGNAL(selectionUpdated()), this, SLOT(selectionUpdatedSlot()));

    setupContextMenu();

    mGoto = 0;
}

void CPUDump::setupContextMenu()
{
    mMenuBuilder = new MenuBuilder(this, [](QMenu*)
    {
        return DbgIsDebugging();
    });

    MenuBuilder* wBinaryMenu = new MenuBuilder(this);
    wBinaryMenu->addAction(makeShortcutAction(DIcon("binary_edit.png"), tr("&Edit"), SLOT(binaryEditSlot()), "ActionBinaryEdit"));
    wBinaryMenu->addAction(makeShortcutAction(DIcon("binary_fill.png"), tr("&Fill..."), SLOT(binaryFillSlot()), "ActionBinaryFill"));
    wBinaryMenu->addSeparator();
    wBinaryMenu->addAction(makeShortcutAction(DIcon("binary_copy.png"), tr("&Copy"), SLOT(binaryCopySlot()), "ActionBinaryCopy"));
    wBinaryMenu->addAction(makeShortcutAction(DIcon("binary_paste.png"), tr("&Paste"), SLOT(binaryPasteSlot()), "ActionBinaryPaste"));
    wBinaryMenu->addAction(makeShortcutAction(DIcon("binary_paste_ignoresize.png"), tr("Paste (&Ignore Size)"), SLOT(binaryPasteIgnoreSizeSlot()), "ActionBinaryPasteIgnoreSize"));
    wBinaryMenu->addAction(makeAction(DIcon("binary_save.png"), tr("Save To a File"), SLOT(binarySaveToFileSlot())));
    mMenuBuilder->addMenu(makeMenu(DIcon("binary.png"), tr("B&inary")), wBinaryMenu);

    MenuBuilder* wCopyMenu = new MenuBuilder(this);
    wCopyMenu->addAction(mCopySelection);
    wCopyMenu->addAction(mCopyAddress);
    wCopyMenu->addAction(mCopyRva, [this](QMenu*)
    {
        return DbgFunctions()->ModBaseFromAddr(rvaToVa(getInitialSelection())) != 0;
    });
    mMenuBuilder->addMenu(makeMenu(DIcon("copy.png"), tr("&Copy")), wCopyMenu);

    mMenuBuilder->addAction(makeShortcutAction(DIcon("eraser.png"), tr("&Restore selection"), SLOT(undoSelectionSlot()), "ActionUndoSelection"), [this](QMenu*)
    {
        return DbgFunctions()->PatchInRange(rvaToVa(getSelectionStart()), rvaToVa(getSelectionEnd()));
    });
    mMenuBuilder->addAction(makeAction(DIcon("stack.png"), tr("Follow in Stack"), SLOT(followStackSlot())), [this](QMenu*)
    {
        auto start = rvaToVa(getSelectionStart());
        return (DbgMemIsValidReadPtr(start) && DbgMemFindBaseAddr(start, 0) == DbgMemFindBaseAddr(DbgValFromString("csp"), 0));
    });
    mMenuBuilder->addAction(makeAction(DIcon("memmap_find_address_page.png"), tr("Follow in Memory Map"), SLOT(followInMemoryMapSlot())));
    mMenuBuilder->addAction(makeAction(DIcon(ArchValue("processor32.png", "processor64.png")), tr("Follow in Disassembler"), SLOT(followInDisasmSlot())));
    auto wIsValidReadPtrCallback = [this](QMenu*)
    {
        duint ptr = 0;
        DbgMemRead(rvaToVa(getSelectionStart()), (unsigned char*)&ptr, sizeof(duint));
        return DbgMemIsValidReadPtr(ptr);
    };

    mMenuBuilder->addAction(makeAction(DIcon("processor32.png"), ArchValue(tr("&Follow DWORD in Disassembler"), tr("&Follow QWORD in Disassembler")), SLOT(followDataSlot())), wIsValidReadPtrCallback);
    mMenuBuilder->addAction(makeAction(DIcon("dump.png"), ArchValue(tr("&Follow DWORD in Current Dump"), tr("&Follow QWORD in Current Dump")), SLOT(followDataDumpSlot())), wIsValidReadPtrCallback);

    MenuBuilder* wFollowInDumpMenu = new MenuBuilder(this, [wIsValidReadPtrCallback, this](QMenu * menu)
    {
        if(!wIsValidReadPtrCallback(menu))
            return false;
        QList<QString> tabNames;
        mMultiDump->getTabNames(tabNames);
        for(int i = 0; i < tabNames.length(); i++)
            mFollowInDumpActions[i]->setText(tabNames[i]);
        return true;
    });
    int maxDumps = mMultiDump->getMaxCPUTabs();
    for(int i = 0; i < maxDumps; i++)
    {
        QAction* action = makeAction(DIcon("dump.png"), QString(), SLOT(followInDumpNSlot()));
        wFollowInDumpMenu->addAction(action);
        mFollowInDumpActions.push_back(action);
    }
    mMenuBuilder->addMenu(makeMenu(DIcon("dump.png"), ArchValue(tr("&Follow DWORD in Dump"), tr("&Follow QWORD in Dump"))), wFollowInDumpMenu);
    mMenuBuilder->addAction(makeShortcutAction(DIcon("label.png"), tr("Set &Label"), SLOT(setLabelSlot()), "ActionSetLabel"));
    mMenuBuilder->addAction(makeAction(DIcon("modify.png"), tr("&Modify Value"), SLOT(modifyValueSlot())), [this](QMenu*)
    {
        return getSizeOf(mDescriptor.at(0).data.itemSize) <= sizeof(duint);
    });

    MenuBuilder* wBreakpointMenu = new MenuBuilder(this);
    MenuBuilder* wHardwareAccessMenu = new MenuBuilder(this, [this](QMenu*)
    {
        return (DbgGetBpxTypeAt(rvaToVa(getSelectionStart())) & bp_hardware) == 0;
    });
    MenuBuilder* wHardwareWriteMenu = new MenuBuilder(this, [this](QMenu*)
    {
        return (DbgGetBpxTypeAt(rvaToVa(getSelectionStart())) & bp_hardware) == 0;
    });
    MenuBuilder* wMemoryAccessMenu = new MenuBuilder(this, [this](QMenu*)
    {
        return (DbgGetBpxTypeAt(rvaToVa(getSelectionStart())) & bp_memory) == 0;
    });
    MenuBuilder* wMemoryWriteMenu = new MenuBuilder(this, [this](QMenu*)
    {
        return (DbgGetBpxTypeAt(rvaToVa(getSelectionStart())) & bp_memory) == 0;
    });
    MenuBuilder* wMemoryExecuteMenu = new MenuBuilder(this, [this](QMenu*)
    {
        return (DbgGetBpxTypeAt(rvaToVa(getSelectionStart())) & bp_memory) == 0;
    });
    wHardwareAccessMenu->addAction(makeAction(DIcon("breakpoint_byte.png"), tr("&Byte"), SLOT(hardwareAccess1Slot())));
    wHardwareAccessMenu->addAction(makeAction(DIcon("breakpoint_word.png"), tr("&Word"), SLOT(hardwareAccess2Slot())));
    wHardwareAccessMenu->addAction(makeAction(DIcon("breakpoint_dword.png"), tr("&Dword"), SLOT(hardwareAccess4Slot())));
#ifdef _WIN64
    wHardwareAccessMenu->addAction(makeAction(DIcon("breakpoint_qword.png"), tr("&Qword"), SLOT(hardwareAccess8Slot())));
#endif //_WIN64
    wHardwareWriteMenu->addAction(makeAction(DIcon("breakpoint_byte.png"), tr("&Byte"), SLOT(hardwareWrite1Slot())));
    wHardwareWriteMenu->addAction(makeAction(DIcon("breakpoint_word.png"), tr("&Word"), SLOT(hardwareWrite2Slot())));
    wHardwareWriteMenu->addAction(makeAction(DIcon("breakpoint_dword.png"), tr("&Dword"), SLOT(hardwareWrite4Slot())));
#ifdef _WIN64
    wHardwareWriteMenu->addAction(makeAction(DIcon("breakpoint_qword.png"), tr("&Qword"), SLOT(hardwareWrite8Slot())));
#endif //_WIN64
    wBreakpointMenu->addMenu(makeMenu(DIcon("breakpoint_access.png"), tr("Hardware, &Access")), wHardwareAccessMenu);
    wBreakpointMenu->addMenu(makeMenu(DIcon("breakpoint_write.png"), tr("Hardware, &Write")), wHardwareWriteMenu);
    wBreakpointMenu->addAction(makeAction(DIcon("breakpoint_execute.png"), tr("Hardware, &Execute"), SLOT(hardwareExecuteSlot())), [this](QMenu*)
    {
        return (DbgGetBpxTypeAt(rvaToVa(getSelectionStart())) & bp_hardware) == 0;
    });
    wBreakpointMenu->addAction(makeAction(DIcon("breakpoint_remove.png"), tr("Remove &Hardware"), SLOT(hardwareRemoveSlot())), [this](QMenu*)
    {
        return (DbgGetBpxTypeAt(rvaToVa(getSelectionStart())) & bp_hardware) != 0;
    });
    wBreakpointMenu->addSeparator();
    wMemoryAccessMenu->addAction(makeAction(DIcon("breakpoint_memory_singleshoot.png"), tr("&Singleshoot"), SLOT(memoryAccessSingleshootSlot())));
    wMemoryAccessMenu->addAction(makeAction(DIcon("breakpoint_memory_restore_on_hit.png"), tr("&Restore on hit"), SLOT(memoryAccessRestoreSlot())));
    wMemoryWriteMenu->addAction(makeAction(DIcon("breakpoint_memory_singleshoot.png"), tr("&Singleshoot"), SLOT(memoryWriteSingleshootSlot())));
    wMemoryWriteMenu->addAction(makeAction(DIcon("breakpoint_memory_restore_on_hit.png"), tr("&Restore on hit"), SLOT(memoryWriteRestoreSlot())));
    wMemoryExecuteMenu->addAction(makeAction(DIcon("breakpoint_memory_singleshoot.png"), tr("&Singleshoot"), SLOT(memoryExecuteSingleshootSlot())));
    wMemoryExecuteMenu->addAction(makeAction(DIcon("breakpoint_memory_restore_on_hit.png"), tr("&Restore on hit"), SLOT(memoryExecuteRestoreSlot())));
    wBreakpointMenu->addMenu(makeMenu(DIcon("breakpoint_memory_access.png"), tr("Memory, Access")), wMemoryAccessMenu);
    wBreakpointMenu->addMenu(makeMenu(DIcon("breakpoint_memory_write.png"), tr("Memory, Write")), wMemoryWriteMenu);
    wBreakpointMenu->addMenu(makeMenu(DIcon("breakpoint_memory_execute.png"), tr("Memory, Execute")), wMemoryExecuteMenu);
    wBreakpointMenu->addAction(makeAction(DIcon("breakpoint_remove.png"), tr("Remove &Memory"), SLOT(memoryRemoveSlot())), [this](QMenu*)
    {
        return (DbgGetBpxTypeAt(rvaToVa(getSelectionStart())) & bp_memory) != 0;
    });
    mMenuBuilder->addMenu(makeMenu(DIcon("breakpoint.png"), tr("&Breakpoint")), wBreakpointMenu);

    mMenuBuilder->addAction(makeShortcutAction(DIcon("search-for.png"), tr("&Find Pattern..."), SLOT(findPattern()), "ActionFindPattern"));
    mMenuBuilder->addAction(makeShortcutAction(DIcon("find.png"), tr("Find &References"), SLOT(findReferencesSlot()), "ActionFindReferences"));
    mMenuBuilder->addAction(makeShortcutAction(DIcon("yara.png"), tr("&Yara..."), SLOT(yaraSlot()), "ActionYara"));
    mMenuBuilder->addAction(makeAction(DIcon("data-copy.png"), tr("Data co&py..."), SLOT(dataCopySlot())));

    mMenuBuilder->addAction(makeShortcutAction(DIcon("sync.png"), tr("&Sync with expression"), SLOT(syncWithExpressionSlot()), "ActionSyncWithExpression"));
    mMenuBuilder->addAction(makeAction(DIcon("animal-dog.png"), ArchValue(tr("Watch DWORD"), tr("Watch QWORD")), SLOT(watchSlot())));
    mMenuBuilder->addAction(makeShortcutAction(DIcon("entropy.png"), tr("Entrop&y..."), SLOT(entropySlot()), "ActionEntropy"));
    mMenuBuilder->addAction(makeAction(tr("Allocate Memory"), SLOT(allocMemorySlot())));

    MenuBuilder* wGotoMenu = new MenuBuilder(this);
    wGotoMenu->addAction(makeShortcutAction(DIcon("geolocation-goto.png"), tr("&Expression"), SLOT(gotoExpressionSlot()), "ActionGotoExpression"));
    wGotoMenu->addAction(makeShortcutAction(DIcon("fileoffset.png"), tr("File Offset"), SLOT(gotoFileOffsetSlot()), "ActionGotoFileOffset"));
    wGotoMenu->addAction(makeShortcutAction(DIcon("top.png"), tr("Start of Page"), SLOT(gotoStartSlot()), "ActionGotoStart"), [this](QMenu*)
    {
        return getSelectionStart() != 0;
    });
    wGotoMenu->addAction(makeShortcutAction(DIcon("bottom.png"), tr("End of Page"), SLOT(gotoEndSlot()), "ActionGotoEnd"));
    wGotoMenu->addAction(makeShortcutAction(DIcon("previous.png"), tr("Previous"), SLOT(gotoPrevSlot()), "ActionGotoPrevious"), [this](QMenu*)
    {
        return historyHasPrev();
    });
    wGotoMenu->addAction(makeShortcutAction(DIcon("next.png"), tr("Next"), SLOT(gotoNextSlot()), "ActionGotoNext"), [this](QMenu*)
    {
        return historyHasNext();
    });
    mMenuBuilder->addMenu(makeMenu(DIcon("goto.png"), tr("&Go to")), wGotoMenu);
    mMenuBuilder->addSeparator();

    MenuBuilder* wHexMenu = new MenuBuilder(this);
    wHexMenu->addAction(makeAction(DIcon("ascii.png"), tr("&ASCII"), SLOT(hexAsciiSlot())));
    wHexMenu->addAction(makeAction(DIcon("ascii-extended.png"), tr("&Extended ASCII"), SLOT(hexUnicodeSlot())));
    QAction* wHexLastCodepage = makeAction(DIcon("codepage.png"), "?", SLOT(hexLastCodepageSlot()));
    wHexMenu->addAction(wHexLastCodepage, [wHexLastCodepage](QMenu*)
    {
        duint lastCodepage;
        auto allCodecs = QTextCodec::availableCodecs();
        if(!BridgeSettingGetUint("Misc", "LastCodepage", &lastCodepage) || lastCodepage >= duint(allCodecs.size()))
            return false;
        wHexLastCodepage->setText(QString::fromLocal8Bit(allCodecs.at(lastCodepage)));
        return true;
    });
    wHexMenu->addAction(makeAction(DIcon("codepage.png"), tr("&Codepage..."), SLOT(hexCodepageSlot())));
    mMenuBuilder->addMenu(makeMenu(DIcon("hex.png"), tr("&Hex")), wHexMenu);

    MenuBuilder* wTextMenu = new MenuBuilder(this);
    wTextMenu->addAction(makeAction(DIcon("ascii.png"), tr("&ASCII"), SLOT(textAsciiSlot())));
    wTextMenu->addAction(makeAction(DIcon("ascii-extended.png"), tr("&Extended ASCII"), SLOT(textUnicodeSlot())));
    QAction* wTextLastCodepage = makeAction(DIcon("codepage.png"), "?", SLOT(textLastCodepageSlot()));
    wTextMenu->addAction(wTextLastCodepage, [wTextLastCodepage](QMenu*)
    {
        duint lastCodepage;
        auto allCodecs = QTextCodec::availableCodecs();
        if(!BridgeSettingGetUint("Misc", "LastCodepage", &lastCodepage) || lastCodepage >= duint(allCodecs.size()))
            return false;
        wTextLastCodepage->setText(QString::fromLocal8Bit(allCodecs.at(lastCodepage)));
        return true;
    });
    wTextMenu->addAction(makeAction(DIcon("codepage.png"), tr("&Codepage..."), SLOT(textCodepageSlot())));
    mMenuBuilder->addMenu(makeMenu(DIcon("strings.png"), tr("&Text")), wTextMenu);

    MenuBuilder* wIntegerMenu = new MenuBuilder(this);
    wIntegerMenu->addAction(makeAction(DIcon("byte.png"), tr("Signed byte (8-bit)"), SLOT(integerSignedByteSlot())));
    wIntegerMenu->addAction(makeAction(DIcon("word.png"), tr("Signed short (16-bit)"), SLOT(integerSignedShortSlot())));
    wIntegerMenu->addAction(makeAction(DIcon("dword.png"), tr("Signed long (32-bit)"), SLOT(integerSignedLongSlot())));
    wIntegerMenu->addAction(makeAction(DIcon("qword.png"), tr("Signed long long (64-bit)"), SLOT(integerSignedLongLongSlot())));
    wIntegerMenu->addAction(makeAction(DIcon("byte.png"), tr("Unsigned byte (8-bit)"), SLOT(integerUnsignedByteSlot())));
    wIntegerMenu->addAction(makeAction(DIcon("word.png"), tr("Unsigned short (16-bit)"), SLOT(integerUnsignedShortSlot())));
    wIntegerMenu->addAction(makeAction(DIcon("dword.png"), tr("Unsigned long (32-bit)"), SLOT(integerUnsignedLongSlot())));
    wIntegerMenu->addAction(makeAction(DIcon("qword.png"), tr("Unsigned long long (64-bit)"), SLOT(integerUnsignedLongLongSlot())));
    wIntegerMenu->addAction(makeAction(DIcon("word.png"), tr("Hex short (16-bit)"), SLOT(integerHexShortSlot())));
    wIntegerMenu->addAction(makeAction(DIcon("dword.png"), tr("Hex long (32-bit)"), SLOT(integerHexLongSlot())));
    wIntegerMenu->addAction(makeAction(DIcon("qword.png"), tr("Hex long long (64-bit)"), SLOT(integerHexLongLongSlot())));
    mMenuBuilder->addMenu(makeMenu(DIcon("integer.png"), tr("&Integer")), wIntegerMenu);

    MenuBuilder* wFloatMenu = new MenuBuilder(this);
    wFloatMenu->addAction(makeAction(DIcon("32bit-float.png"), tr("&Float (32-bit)"), SLOT(floatFloatSlot())));
    wFloatMenu->addAction(makeAction(DIcon("64bit-float.png"), tr("&Double (64-bit)"), SLOT(floatDoubleSlot())));
    wFloatMenu->addAction(makeAction(DIcon("80bit-float.png"), tr("&Long double (80-bit)"), SLOT(floatLongDoubleSlot())));
    mMenuBuilder->addMenu(makeMenu(DIcon("float.png"), tr("&Float")), wFloatMenu);

    mMenuBuilder->addAction(makeAction(DIcon("address.png"), tr("&Address"), SLOT(addressSlot())));
    mMenuBuilder->addAction(makeAction(DIcon("processor-cpu.png"), tr("&Disassembly"), SLOT(disassemblySlot())))->setEnabled(false);

    mPluginMenu = new QMenu(this);
    mPluginMenu->setIcon(DIcon("plugin.png"));
    Bridge::getBridge()->emitMenuAddToList(this, mPluginMenu, GUI_DUMP_MENU);
    mMenuBuilder->addSeparator();
    mMenuBuilder->addBuilder(new MenuBuilder(this, [this](QMenu * menu)
    {
        menu->addActions(mPluginMenu->actions());
        return true;
    }));

    updateShortcuts();
}

void CPUDump::getColumnRichText(int col, dsint rva, RichTextPainter::List & richText)
{
    if(col && !mDescriptor.at(col - 1).isData && mDescriptor.at(col - 1).itemCount) //print comments
    {
        RichTextPainter::CustomRichText_t curData;
        curData.flags = RichTextPainter::FlagColor;
        curData.textColor = textColor;
        duint data = 0;
        mMemPage->read((byte_t*)&data, rva, sizeof(duint));

        char modname[MAX_MODULE_SIZE] = "";
        if(!DbgGetModuleAt(data, modname))
            modname[0] = '\0';
        char label_text[MAX_LABEL_SIZE] = "";
        if(DbgGetLabelAt(data, SEG_DEFAULT, label_text))
            curData.text = QString(modname) + "." + QString(label_text);
        char string_text[MAX_STRING_SIZE] = "";
        if(DbgGetStringAt(data, string_text))
            curData.text = string_text;
        if(!curData.text.length()) //stack comments
        {
            auto va = rvaToVa(rva);
            duint stackSize;
            duint csp = DbgValFromString("csp");
            duint stackBase = DbgMemFindBaseAddr(csp, &stackSize);
            STACK_COMMENT comment;
            if(va >= stackBase && va < stackBase + stackSize && DbgStackCommentGet(va, &comment))
            {
                if(va >= csp) //active stack
                {
                    if(*comment.color)
                        curData.textColor = QColor(QString(comment.color));
                }
                else
                    curData.textColor = ConfigColor("StackInactiveTextColor");
                curData.text = comment.comment;
            }
        }
        if(curData.text.length())
            richText.push_back(curData);
    }
    else
        HexDump::getColumnRichText(col, rva, richText);
}

QString CPUDump::paintContent(QPainter* painter, dsint rowBase, int rowOffset, int col, int x, int y, int w, int h)
{
    // Reset byte offset when base address is reached
    if(rowBase == 0 && mByteOffset != 0)
        printDumpAt(mMemPage->getBase(), false, false);

    if(!col) //address
    {
        char label[MAX_LABEL_SIZE] = "";
        dsint cur_addr = rvaToVa((rowBase + rowOffset) * getBytePerRowCount() - mByteOffset);
        QColor background;
        if(DbgGetLabelAt(cur_addr, SEG_DEFAULT, label)) //label
        {
            background = ConfigColor("HexDumpLabelBackgroundColor");
            painter->setPen(ConfigColor("HexDumpLabelColor")); //TODO: config
        }
        else
        {
            background = ConfigColor("HexDumpAddressBackgroundColor");
            painter->setPen(ConfigColor("HexDumpAddressColor")); //TODO: config
        }
        if(background.alpha())
            painter->fillRect(QRect(x, y, w, h), QBrush(background)); //fill background color
        painter->drawText(QRect(x + 4, y , w - 4 , h), Qt::AlignVCenter | Qt::AlignLeft, makeAddrText(cur_addr));
        return "";
    }
    return HexDump::paintContent(painter, rowBase, rowOffset, col, x, y, w, h);
}

void CPUDump::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu wMenu(this);
    mMenuBuilder->build(&wMenu);
    wMenu.exec(event->globalPos());
}

void CPUDump::mouseDoubleClickEvent(QMouseEvent* event)
{
    if(event->button() != Qt::LeftButton || !DbgIsDebugging())
        return;
    switch(getColumnIndexFromX(event->x()))
    {
    case 0: //address
    {
        //very ugly way to calculate the base of the current row (no clue why it works)
        dsint deltaRowBase = getInitialSelection() % getBytePerRowCount() + mByteOffset;
        if(deltaRowBase >= getBytePerRowCount())
            deltaRowBase -= getBytePerRowCount();
        dsint mSelectedVa = rvaToVa(getInitialSelection() - deltaRowBase);
        if(mRvaDisplayEnabled && mSelectedVa == mRvaDisplayBase)
            mRvaDisplayEnabled = false;
        else
        {
            mRvaDisplayEnabled = true;
            mRvaDisplayBase = mSelectedVa;
            mRvaDisplayPageBase = mMemPage->getBase();
        }
        reloadData();
    }
    break;

    default:
    {
        if(getSizeOf(mDescriptor.at(0).data.itemSize) <= sizeof(duint))
            modifyValueSlot();
        else
            binaryEditSlot();
    }
    break;
    }
}

void CPUDump::mouseMoveEvent(QMouseEvent* event)
{
    dsint ptr = 0, ptrValue = 0;

    // Get mouse pointer relative position
    int x = event->x();
    int y = event->y();

    // Get HexDump own RVA address, then VA in memory
    int rva = getItemStartingAddress(x, y);
    int va = rvaToVa(rva);

    // Read VA
    DbgMemRead(va, (unsigned char*)&ptr, sizeof(dsint));

    // Check if its a pointer
    if(DbgMemIsValidReadPtr(ptr))
    {
        // Get the value at the address pointed by the ptr
        DbgMemRead(ptr, (unsigned char*)&ptrValue, sizeof(dsint));

        // Format text like this : [ptr] = ptrValue
        QString strPtrValue;
#ifdef _WIN64
        strPtrValue.sprintf("[0x%016X] = 0x%016X", ptr, ptrValue);
#else
        strPtrValue.sprintf("[0x%08X] = 0x%08X", ptr, ptrValue);
#endif

        // Show tooltip
        QToolTip::showText(event->globalPos(), strPtrValue);
    }
    // If not a pointer, hide tooltips
    else
    {
        QToolTip::hideText();
    }

    HexDump::mouseMoveEvent(event);
}

void CPUDump::setLabelSlot()
{
    if(!DbgIsDebugging())
        return;

    duint wVA = rvaToVa(getSelectionStart());
    LineEditDialog mLineEdit(this);
    QString addr_text = QString("%1").arg(wVA, sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    char label_text[MAX_COMMENT_SIZE] = "";
    if(DbgGetLabelAt((duint)wVA, SEG_DEFAULT, label_text))
        mLineEdit.setText(QString(label_text));
    mLineEdit.setWindowTitle(tr("Add label at ") + addr_text);
    if(mLineEdit.exec() != QDialog::Accepted)
        return;
    if(!DbgSetLabelAt(wVA, mLineEdit.editText.toUtf8().constData()))
        SimpleErrorBox(this, tr("Error!"), tr("DbgSetLabelAt failed!"));
    GuiUpdateAllViews();
}

void CPUDump::modifyValueSlot()
{
    dsint addr = getSelectionStart();
    WordEditDialog wEditDialog(this);
    dsint value = 0;
    auto size = std::min(getSizeOf(mDescriptor.at(0).data.itemSize), int(sizeof(dsint)));
    mMemPage->read(&value, addr, size);
    wEditDialog.setup(tr("Modify value"), value, size);
    if(wEditDialog.exec() != QDialog::Accepted)
        return;
    value = wEditDialog.getVal();
    mMemPage->write(&value, addr, size);
    GuiUpdateAllViews();
}

void CPUDump::gotoExpressionSlot()
{
    if(!DbgIsDebugging())
        return;
    if(!mGoto)
        mGoto = new GotoDialog(this);
    mGoto->setWindowTitle(tr("Enter expression to follow in Dump..."));
    if(mGoto->exec() == QDialog::Accepted)
    {
        duint value = DbgValFromString(mGoto->expressionText.toUtf8().constData());
        DbgCmdExec(QString().sprintf("dump %p", value).toUtf8().constData());
    }
}

void CPUDump::gotoFileOffsetSlot()
{
    if(!DbgIsDebugging())
        return;
    char modname[MAX_MODULE_SIZE] = "";
    if(!DbgFunctions()->ModNameFromAddr(rvaToVa(getSelectionStart()), modname, true))
    {
        SimpleErrorBox(this, tr("Error!"), tr("Not inside a module..."));
        return;
    }
    GotoDialog mGotoDialog(this);
    mGotoDialog.fileOffset = true;
    mGotoDialog.modName = QString(modname);
    mGotoDialog.setWindowTitle(tr("Goto File Offset in %1").arg(QString(modname)));
    if(mGotoDialog.exec() != QDialog::Accepted)
        return;
    duint value = DbgValFromString(mGotoDialog.expressionText.toUtf8().constData());
    value = DbgFunctions()->FileOffsetToVa(modname, value);
    DbgCmdExec(QString().sprintf("dump \"%p\"", value).toUtf8().constData());
}

void CPUDump::gotoStartSlot()
{
    duint dest = mMemPage->getBase();
    DbgCmdExec(QString().sprintf("dump \"%p\"", dest).toUtf8().constData());
}

void CPUDump::gotoEndSlot()
{
    duint dest = mMemPage->getBase() + mMemPage->getSize() - (getViewableRowsCount() * getBytePerRowCount());
    DbgCmdExec(QString().sprintf("dump \"%p\"", dest).toUtf8().constData());
}

void CPUDump::hexAsciiSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewHexAscii);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //hex byte
    wColDesc.itemCount = 16;
    wColDesc.separator = 4;
    dDesc.itemSize = Byte;
    dDesc.byteMode = HexByte;
    wColDesc.data = dDesc;
    appendResetDescriptor(8 + charwidth * 47, tr("Hex"), false, wColDesc);

    wColDesc.isData = true; //ascii byte
    wColDesc.itemCount = 16;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(8 + charwidth * 16, tr("ASCII"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::hexUnicodeSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewHexUnicode);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //hex byte
    wColDesc.itemCount = 16;
    wColDesc.separator = 4;
    dDesc.itemSize = Byte;
    dDesc.byteMode = HexByte;
    wColDesc.data = dDesc;
    appendResetDescriptor(8 + charwidth * 47, tr("Hex"), false, wColDesc);

    wColDesc.isData = true; //unicode short
    wColDesc.itemCount = 8;
    wColDesc.separator = 0;
    dDesc.itemSize = Word;
    dDesc.wordMode = UnicodeWord;
    wColDesc.data = dDesc;
    appendDescriptor(8 + charwidth * 8, tr("UNICODE"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::hexCodepageSlot()
{
    CodepageSelectionDialog dialog(this);
    if(dialog.exec() != QDialog::Accepted)
        return;
    auto codepage = dialog.getSelectedCodepage();

    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //hex byte
    wColDesc.itemCount = 16;
    wColDesc.separator = 4;
    dDesc.itemSize = Byte;
    dDesc.byteMode = HexByte;
    wColDesc.data = dDesc;
    appendResetDescriptor(8 + charwidth * 47, tr("Hex"), false, wColDesc);

    wColDesc.isData = true; //text (in code page)
    wColDesc.itemCount = 16;
    wColDesc.separator = 0;
    wColDesc.textCodec = QTextCodec::codecForName(codepage);
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, codepage, false, wColDesc);

    reloadData();
}

void CPUDump::hexLastCodepageSlot()
{
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;
    duint lastCodepage;
    auto allCodecs = QTextCodec::availableCodecs();
    if(!BridgeSettingGetUint("Misc", "LastCodepage", &lastCodepage) || lastCodepage >= duint(allCodecs.size()))
        return;

    wColDesc.isData = true; //hex byte
    wColDesc.itemCount = 16;
    wColDesc.separator = 4;
    dDesc.itemSize = Byte;
    dDesc.byteMode = HexByte;
    wColDesc.data = dDesc;
    appendResetDescriptor(8 + charwidth * 47, tr("Hex"), false, wColDesc);

    wColDesc.isData = true; //text (in code page)
    wColDesc.itemCount = 16;
    wColDesc.separator = 0;
    wColDesc.textCodec = QTextCodec::codecForName(allCodecs.at(lastCodepage));
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, allCodecs.at(lastCodepage), false, wColDesc);

    reloadData();
}

void CPUDump::textLastCodepageSlot()
{
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;
    duint lastCodepage;
    auto allCodecs = QTextCodec::availableCodecs();
    if(!BridgeSettingGetUint("Misc", "LastCodepage", &lastCodepage) || lastCodepage >= duint(allCodecs.size()))
        return;

    wColDesc.isData = true; //text (in code page)
    wColDesc.itemCount = 64;
    wColDesc.separator = 0;
    wColDesc.textCodec = QTextCodec::codecForName(allCodecs.at(lastCodepage));
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendResetDescriptor(0, allCodecs.at(lastCodepage), false, wColDesc);

    reloadData();
}

void CPUDump::textAsciiSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewTextAscii);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //ascii byte
    wColDesc.itemCount = 64;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendResetDescriptor(8 + charwidth * 64, tr("ASCII"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::textUnicodeSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewTextUnicode);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //unicode short
    wColDesc.itemCount = 64;
    wColDesc.separator = 0;
    dDesc.itemSize = Word;
    dDesc.wordMode = UnicodeWord;
    wColDesc.data = dDesc;
    appendResetDescriptor(8 + charwidth * 64, tr("UNICODE"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::textCodepageSlot()
{
    CodepageSelectionDialog dialog(this);
    if(dialog.exec() != QDialog::Accepted)
        return;
    auto codepage = dialog.getSelectedCodepage();

    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //text (in code page)
    wColDesc.itemCount = 64;
    wColDesc.separator = 0;
    wColDesc.textCodec = QTextCodec::codecForName(codepage);
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendResetDescriptor(0, codepage, false, wColDesc);

    reloadData();
}

void CPUDump::integerSignedByteSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewIntegerSignedByte);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //signed short
    wColDesc.itemCount = 8;
    wColDesc.separator = 0;
    wColDesc.data.itemSize = Byte;
    wColDesc.data.wordMode = SignedDecWord;
    appendResetDescriptor(8 + charwidth * 40, tr("Signed byte (8-bit)"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::integerSignedShortSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewIntegerSignedShort);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //signed short
    wColDesc.itemCount = 8;
    wColDesc.separator = 0;
    wColDesc.data.itemSize = Word;
    wColDesc.data.wordMode = SignedDecWord;
    appendResetDescriptor(8 + charwidth * 55, tr("Signed short (16-bit)"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::integerSignedLongSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewIntegerSignedLong);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //signed long
    wColDesc.itemCount = 4;
    wColDesc.separator = 0;
    wColDesc.data.itemSize = Dword;
    wColDesc.data.dwordMode = SignedDecDword;
    appendResetDescriptor(8 + charwidth * 47, tr("Signed long (32-bit)"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::integerSignedLongLongSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewIntegerSignedLongLong);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //signed long long
    wColDesc.itemCount = 2;
    wColDesc.separator = 0;
    wColDesc.data.itemSize = Qword;
    wColDesc.data.qwordMode = SignedDecQword;
    appendResetDescriptor(8 + charwidth * 41, tr("Signed long long (64-bit)"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::integerUnsignedByteSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewIntegerUnsignedByte);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //unsigned short
    wColDesc.itemCount = 8;
    wColDesc.separator = 0;
    wColDesc.data.itemSize = Byte;
    wColDesc.data.wordMode = UnsignedDecWord;
    appendResetDescriptor(8 + charwidth * 32, tr("Unsigned byte (8-bit)"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::integerUnsignedShortSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewIntegerUnsignedShort);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //unsigned short
    wColDesc.itemCount = 8;
    wColDesc.separator = 0;
    wColDesc.data.itemSize = Word;
    wColDesc.data.wordMode = UnsignedDecWord;
    appendResetDescriptor(8 + charwidth * 47, tr("Unsigned short (16-bit)"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::integerUnsignedLongSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewIntegerUnsignedLong);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //unsigned long
    wColDesc.itemCount = 4;
    wColDesc.separator = 0;
    wColDesc.data.itemSize = Dword;
    wColDesc.data.dwordMode = UnsignedDecDword;
    appendResetDescriptor(8 + charwidth * 43, tr("Unsigned long (32-bit)"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::integerUnsignedLongLongSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewIntegerUnsignedLongLong);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //unsigned long long
    wColDesc.itemCount = 2;
    wColDesc.separator = 0;
    wColDesc.data.itemSize = Qword;
    wColDesc.data.qwordMode = UnsignedDecQword;
    appendResetDescriptor(8 + charwidth * 41, tr("Unsigned long long (64-bit)"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::integerHexShortSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewIntegerHexShort);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //hex short
    wColDesc.itemCount = 8;
    wColDesc.separator = 0;
    wColDesc.data.itemSize = Word;
    wColDesc.data.wordMode = HexWord;
    appendResetDescriptor(8 + charwidth * 34, tr("Hex short (16-bit)"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::integerHexLongSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewIntegerHexLong);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //hex long
    wColDesc.itemCount = 4;
    wColDesc.separator = 0;
    wColDesc.data.itemSize = Dword;
    wColDesc.data.dwordMode = HexDword;
    appendResetDescriptor(8 + charwidth * 35, tr("Hex long (32-bit)"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::integerHexLongLongSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewIntegerHexLongLong);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //hex long long
    wColDesc.itemCount = 2;
    wColDesc.separator = 0;
    wColDesc.data.itemSize = Qword;
    wColDesc.data.qwordMode = HexQword;
    appendResetDescriptor(8 + charwidth * 33, tr("Hex long long (64-bit)"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::floatFloatSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewFloatFloat);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //float dword
    wColDesc.itemCount = 4;
    wColDesc.separator = 0;
    wColDesc.data.itemSize = Dword;
    wColDesc.data.dwordMode = FloatDword;
    appendResetDescriptor(8 + charwidth * 55, tr("Float (32-bit)"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::floatDoubleSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewFloatDouble);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //float qword
    wColDesc.itemCount = 2;
    wColDesc.separator = 0;
    wColDesc.data.itemSize = Qword;
    wColDesc.data.qwordMode = DoubleQword;
    appendResetDescriptor(8 + charwidth * 47, tr("Double (64-bit)"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::floatLongDoubleSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewFloatLongDouble);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //float qword
    wColDesc.itemCount = 2;
    wColDesc.separator = 0;
    wColDesc.data.itemSize = Tword;
    wColDesc.data.twordMode = FloatTword;
    appendResetDescriptor(8 + charwidth * 59, tr("Long double (80-bit)"), false, wColDesc);

    wColDesc.isData = false; //empty column
    wColDesc.itemCount = 0;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, "", false, wColDesc);

    reloadData();
}

void CPUDump::addressSlot()
{
    Config()->setUint("HexDump", "DefaultView", (duint)ViewAddress);
    int charwidth = getCharWidth();
    ColumnDescriptor_t wColDesc;
    DataDescriptor_t dDesc;

    wColDesc.isData = true; //void*
    wColDesc.itemCount = 1;
    wColDesc.separator = 0;
#ifdef _WIN64
    wColDesc.data.itemSize = Qword;
    wColDesc.data.qwordMode = HexQword;
#else
    wColDesc.data.itemSize = Dword;
    wColDesc.data.dwordMode = HexDword;
#endif
    appendResetDescriptor(8 + charwidth * 2 * sizeof(duint), tr("Address"), false, wColDesc);

    wColDesc.isData = false; //comments
    wColDesc.itemCount = 1;
    wColDesc.separator = 0;
    dDesc.itemSize = Byte;
    dDesc.byteMode = AsciiByte;
    wColDesc.data = dDesc;
    appendDescriptor(0, tr("Comments"), false, wColDesc);

    reloadData();
}

void CPUDump::disassemblySlot()
{
    SimpleErrorBox(this, tr("Error!"), tr("Not yet supported!"));
}

void CPUDump::selectionGet(SELECTIONDATA* selection)
{
    selection->start = rvaToVa(getSelectionStart());
    selection->end = rvaToVa(getSelectionEnd());
    Bridge::getBridge()->setResult(1);
}

void CPUDump::selectionSet(const SELECTIONDATA* selection)
{
    dsint selMin = mMemPage->getBase();
    dsint selMax = selMin + mMemPage->getSize();
    dsint start = selection->start;
    dsint end = selection->end;
    if(start < selMin || start >= selMax || end < selMin || end >= selMax) //selection out of range
    {
        Bridge::getBridge()->setResult(0);
        return;
    }
    setSingleSelection(start - selMin);
    expandSelectionUpTo(end - selMin);
    reloadData();
    Bridge::getBridge()->setResult(1);
}

void CPUDump::memoryAccessSingleshootSlot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bpm " + addr_text + ", 0, r").toUtf8().constData());
}

void CPUDump::memoryAccessRestoreSlot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bpm " + addr_text + ", 1, r").toUtf8().constData());
}

void CPUDump::memoryWriteSingleshootSlot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bpm " + addr_text + ", 0, w").toUtf8().constData());
}

void CPUDump::memoryWriteRestoreSlot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bpm " + addr_text + ", 1, w").toUtf8().constData());
}

void CPUDump::memoryExecuteSingleshootSlot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bpm " + addr_text + ", 0, x").toUtf8().constData());
}

void CPUDump::memoryExecuteRestoreSlot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bpm " + addr_text + ", 1, x").toUtf8().constData());
}

void CPUDump::memoryRemoveSlot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bpmc " + addr_text).toUtf8().constData());
}

void CPUDump::hardwareAccess1Slot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bphws " + addr_text + ", r, 1").toUtf8().constData());
}

void CPUDump::hardwareAccess2Slot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bphws " + addr_text + ", r, 2").toUtf8().constData());
}

void CPUDump::hardwareAccess4Slot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bphws " + addr_text + ", r, 4").toUtf8().constData());
}

void CPUDump::hardwareAccess8Slot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bphws " + addr_text + ", r, 8").toUtf8().constData());
}

void CPUDump::hardwareWrite1Slot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bphws " + addr_text + ", w, 1").toUtf8().constData());
}

void CPUDump::hardwareWrite2Slot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bphws " + addr_text + ", w, 2").toUtf8().constData());
}

void CPUDump::hardwareWrite4Slot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bphws " + addr_text + ", w, 4").toUtf8().constData());
}

void CPUDump::hardwareWrite8Slot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bphws " + addr_text + ", w, 8").toUtf8().constData());
}

void CPUDump::hardwareExecuteSlot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bphws " + addr_text + ", x").toUtf8().constData());
}

void CPUDump::hardwareRemoveSlot()
{
    QString addr_text = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("bphwc " + addr_text).toUtf8().constData());
}

void CPUDump::findReferencesSlot()
{
    QString addrStart = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    QString addrEnd = QString("%1").arg(rvaToVa(getSelectionEnd()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    QString addrDisasm = QString("%1").arg(mDisas->rvaToVa(mDisas->getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("findrefrange " + addrStart + ", " + addrEnd + ", " + addrDisasm).toUtf8().constData());
    emit displayReferencesWidget();
}

void CPUDump::binaryEditSlot()
{
    HexEditDialog hexEdit(this);
    dsint selStart = getSelectionStart();
    dsint selSize = getSelectionEnd() - selStart + 1;
    byte_t* data = new byte_t[selSize];
    mMemPage->read(data, selStart, selSize);
    hexEdit.mHexEdit->setData(QByteArray((const char*)data, selSize));
    delete [] data;
    hexEdit.setWindowTitle(tr("Edit data at %1").arg(ToPtrString(rvaToVa(selStart))));
    if(hexEdit.exec() != QDialog::Accepted)
        return;
    dsint dataSize = hexEdit.mHexEdit->data().size();
    dsint newSize = selSize > dataSize ? selSize : dataSize;
    data = new byte_t[newSize];
    mMemPage->read(data, selStart, newSize);
    QByteArray patched = hexEdit.mHexEdit->applyMaskedData(QByteArray((const char*)data, newSize));
    mMemPage->write(patched.constData(), selStart, patched.size());
    GuiUpdateAllViews();
}

void CPUDump::binaryFillSlot()
{
    HexEditDialog hexEdit(this);
    hexEdit.showKeepSize(false);
    hexEdit.mHexEdit->setOverwriteMode(false);
    dsint selStart = getSelectionStart();
    hexEdit.setWindowTitle(tr("Fill data at %1").arg(rvaToVa(selStart), sizeof(dsint) * 2, 16, QChar('0')).toUpper());
    if(hexEdit.exec() != QDialog::Accepted)
        return;
    QString pattern = hexEdit.mHexEdit->pattern();
    dsint selSize = getSelectionEnd() - selStart + 1;
    byte_t* data = new byte_t[selSize];
    mMemPage->read(data, selStart, selSize);
    hexEdit.mHexEdit->setData(QByteArray((const char*)data, selSize));
    delete [] data;
    hexEdit.mHexEdit->fill(0, QString(pattern));
    QByteArray patched(hexEdit.mHexEdit->data());
    mMemPage->write(patched, selStart, patched.size());
    GuiUpdateAllViews();
}

void CPUDump::binaryCopySlot()
{
    HexEditDialog hexEdit(this);
    dsint selStart = getSelectionStart();
    dsint selSize = getSelectionEnd() - selStart + 1;
    byte_t* data = new byte_t[selSize];
    mMemPage->read(data, selStart, selSize);
    hexEdit.mHexEdit->setData(QByteArray((const char*)data, selSize));
    delete [] data;
    Bridge::CopyToClipboard(hexEdit.mHexEdit->pattern(true));
}

void CPUDump::binaryPasteSlot()
{
    HexEditDialog hexEdit(this);
    dsint selStart = getSelectionStart();
    dsint selSize = getSelectionEnd() - selStart + 1;
    QClipboard* clipboard = QApplication::clipboard();
    hexEdit.mHexEdit->setData(clipboard->text());

    byte_t* data = new byte_t[selSize];
    mMemPage->read(data, selStart, selSize);
    QByteArray patched = hexEdit.mHexEdit->applyMaskedData(QByteArray((const char*)data, selSize));
    if(patched.size() < selSize)
        selSize = patched.size();
    mMemPage->write(patched.constData(), selStart, selSize);
    GuiUpdateAllViews();
}

void CPUDump::binaryPasteIgnoreSizeSlot()
{
    HexEditDialog hexEdit(this);
    dsint selStart = getSelectionStart();
    dsint selSize = getSelectionEnd() - selStart + 1;
    QClipboard* clipboard = QApplication::clipboard();
    hexEdit.mHexEdit->setData(clipboard->text());

    byte_t* data = new byte_t[selSize];
    mMemPage->read(data, selStart, selSize);
    QByteArray patched = hexEdit.mHexEdit->applyMaskedData(QByteArray((const char*)data, selSize));
    delete [] data;
    mMemPage->write(patched.constData(), selStart, patched.size());
    GuiUpdateAllViews();
}

void CPUDump::binarySaveToFileSlot()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save to file"), QDir::currentPath(), tr("All files (*.*)"));
    if(fileName.length())
    {
        // Get starting selection and selection size, then convert selStart to VA
        dsint selStart = getSelectionStart();
        dsint selSize = getSelectionEnd() - selStart + 1;

        // Prepare command
        fileName = QDir::toNativeSeparators(fileName);
        QString cmd = QString("savedata \"%1\",%2,%3").arg(fileName, ToHexString(rvaToVa(selStart)), ToHexString(selSize));
        DbgCmdExec(cmd.toUtf8().constData());
    }
}

void CPUDump::findPattern()
{
    HexEditDialog hexEdit(this);
    hexEdit.showEntireBlock(true);
    hexEdit.mHexEdit->setOverwriteMode(false);
    hexEdit.setWindowTitle(tr("Find Pattern..."));
    if(hexEdit.exec() != QDialog::Accepted)
        return;
    dsint addr = rvaToVa(getSelectionStart());
    if(hexEdit.entireBlock())
        addr = DbgMemFindBaseAddr(addr, 0);
    QString addrText = QString("%1").arg(addr, sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("findall " + addrText + ", " + hexEdit.mHexEdit->pattern() + ", &data&").toUtf8().constData());
    emit displayReferencesWidget();
}

void CPUDump::undoSelectionSlot()
{
    dsint start = rvaToVa(getSelectionStart());
    dsint end = rvaToVa(getSelectionEnd());
    if(!DbgFunctions()->PatchInRange(start, end)) //nothing patched in selected range
        return;
    DbgFunctions()->PatchRestoreRange(start, end);
    reloadData();
}

void CPUDump::followStackSlot()
{
    DbgCmdExec(QString("sdump " + ToPtrString(rvaToVa(getSelectionStart()))).toUtf8().constData());
}

void CPUDump::followInDisasmSlot()
{
    DbgCmdExec(QString("disasm " + ToPtrString(rvaToVa(getSelectionStart()))).toUtf8().constData());
}

void CPUDump::followDataSlot()
{
    DbgCmdExec(QString("disasm \"[%1]\"").arg(ToPtrString(rvaToVa(getSelectionStart()))).toUtf8().constData());
}

void CPUDump::followDataDumpSlot()
{
    DbgCmdExec(QString("dump \"[%1]\"").arg(ToPtrString(rvaToVa(getSelectionStart()))).toUtf8().constData());
}

void CPUDump::selectionUpdatedSlot()
{
    QString selStart = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    QString selEnd = QString("%1").arg(rvaToVa(getSelectionEnd()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    QString info = tr("Dump");
    char mod[MAX_MODULE_SIZE] = "";
    if(DbgFunctions()->ModNameFromAddr(rvaToVa(getSelectionStart()), mod, true))
        info = QString(mod) + "";
    GuiAddStatusBarMessage(QString(info + ": " + selStart + " -> " + selEnd + QString().sprintf(" (0x%.8X bytes)\n", getSelectionEnd() - getSelectionStart() + 1)).toUtf8().constData());
}

void CPUDump::yaraSlot()
{
    YaraRuleSelectionDialog yaraDialog(this);
    if(yaraDialog.exec() == QDialog::Accepted)
    {
        QString addrText = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
        DbgCmdExec(QString("yara \"%0\",%1").arg(yaraDialog.getSelectedFile()).arg(addrText).toUtf8().constData());
        emit displayReferencesWidget();
    }
}

void CPUDump::dataCopySlot()
{
    dsint selStart = getSelectionStart();
    dsint selSize = getSelectionEnd() - selStart + 1;
    QVector<byte_t> data;
    data.resize(selSize);
    mMemPage->read(data.data(), selStart, selSize);
    DataCopyDialog dataDialog(&data, this);
    dataDialog.exec();
}

void CPUDump::entropySlot()
{
    dsint selStart = getSelectionStart();
    dsint selSize = getSelectionEnd() - selStart + 1;
    QVector<byte_t> data;
    data.resize(selSize);
    mMemPage->read(data.data(), selStart, selSize);

    EntropyDialog entropyDialog(this);
    entropyDialog.setWindowTitle(tr("Entropy (Address: %1, Size: %2)").arg(ToPtrString(rvaToVa(selStart))).arg(ToHexString(selSize)));
    entropyDialog.show();
    entropyDialog.GraphMemory(data.constData(), data.size());
    entropyDialog.exec();
}

void CPUDump::syncWithExpressionSlot()
{
    if(!DbgIsDebugging())
        return;
    GotoDialog gotoDialog(this, true);
    gotoDialog.setWindowTitle(tr("Enter expression to sync with..."));
    gotoDialog.setInitialExpression(mSyncAddrExpression);
    if(gotoDialog.exec() != QDialog::Accepted)
        return;
    mSyncAddrExpression = gotoDialog.expressionText;
    updateDumpSlot();
}

void CPUDump::followInDumpNSlot()
{
    for(int i = 0; i < mFollowInDumpActions.length(); i++)
        if(mFollowInDumpActions[i] == sender())
            DbgCmdExec(QString("dump \"[%1]\", \"%2\"").arg(ToPtrString(rvaToVa(getSelectionStart()))).arg(i + 1).toUtf8().constData());
}

void CPUDump::watchSlot()
{
    DbgCmdExec(QString("AddWatch \"[%1]\", \"uint\"").arg(ToPtrString(rvaToVa(getSelectionStart()))).toUtf8().constData());
}

void CPUDump::allocMemorySlot()
{
    WordEditDialog mLineEdit(this);
    mLineEdit.setup(tr("Size"), 0x1000, sizeof(duint));
    if(mLineEdit.exec() == QDialog::Accepted)
    {
        duint memsize = mLineEdit.getVal();
        if(memsize == 0) // 1GB
        {
            SimpleWarningBox(this, tr("Warning"), tr("You're trying to allocate a zero-sized buffer just now."));
            return;
        }
        if(memsize > 1024 * 1024 * 1024)
        {
            SimpleErrorBox(this, tr("Error"), tr("The size of buffer you're trying to allocate exceeds 1GB. Please check your expression to ensure nothing is wrong."));
            return;
        }
        DbgCmdExecDirect(QString("alloc %1").arg(ToPtrString(memsize)).toUtf8().constData());
        duint addr = DbgValFromString("$result");
        if(addr != 0)
        {
            DbgCmdExec("Dump $result");
        }
        else
        {
            SimpleErrorBox(this, tr("Error"), tr("Memory allocation failed!"));
            return;
        }
    }
}

void CPUDump::gotoNextSlot()
{
    historyNext();
}

void CPUDump::gotoPrevSlot()
{
    historyPrev();
}

void CPUDump::setView(ViewEnum_t view)
{
    switch(view)
    {
    case ViewHexAscii:
        hexAsciiSlot();
        break;
    case ViewHexUnicode:
        hexUnicodeSlot();
        break;
    case ViewTextAscii:
        textAsciiSlot();
        break;
    case ViewTextUnicode:
        textUnicodeSlot();
        break;
    case ViewIntegerSignedByte:
        integerSignedByteSlot();
        break;
    case ViewIntegerSignedShort:
        integerSignedShortSlot();
        break;
    case ViewIntegerSignedLong:
        integerSignedLongSlot();
        break;
    case ViewIntegerSignedLongLong:
        integerSignedLongLongSlot();
        break;
    case ViewIntegerUnsignedByte:
        integerUnsignedByteSlot();
        break;
    case ViewIntegerUnsignedShort:
        integerUnsignedShortSlot();
        break;
    case ViewIntegerUnsignedLong:
        integerUnsignedLongSlot();
        break;
    case ViewIntegerUnsignedLongLong:
        integerUnsignedLongLongSlot();
        break;
    case ViewIntegerHexShort:
        integerHexShortSlot();
        break;
    case ViewIntegerHexLong:
        integerHexLongSlot();
        break;
    case ViewIntegerHexLongLong:
        integerHexLongLongSlot();
        break;
    case ViewFloatFloat:
        floatFloatSlot();
        break;
    case ViewFloatDouble:
        floatDoubleSlot();
        break;
    case ViewFloatLongDouble:
        floatLongDoubleSlot();
        break;
    case ViewAddress:
        addressSlot();
        break;
    default:
        hexAsciiSlot();
        break;
    }
}

void CPUDump::followInMemoryMapSlot()
{
    DbgCmdExec(QString("memmapdump %1").arg(ToHexString(rvaToVa(getSelectionStart()))).toUtf8().constData());
}
