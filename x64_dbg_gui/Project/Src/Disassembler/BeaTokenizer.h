#ifndef BEATOKENIZER_H
#define BEATOKENIZER_H

#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QMap>
#include "BeaEngine.h"
#include "NewTypes.h"
#include "RichTextPainter.h"

class BeaTokenizer : RichTextPainter
{
public:
    BeaTokenizer();

    enum BeaTokenType
    {
        //filling
        TokenComma,
        TokenSpace,
        TokenArgumentSpace,
        TokenMemoryOperatorSpace,
        //general instruction parts
        TokenPrefix,
        TokenUncategorized,
        TokenAddress, //jump/call destinations or displacements inside memory
        TokenValue,
        //mnemonics
        TokenMnemonicNormal,
        TokenMnemonicPushPop,
        TokenMnemonicCallRet,
        TokenMnemonicCondJump,
        TokenMnemonicUncondJump,
        TokenMnemonicNop,
        //memory
        TokenMemorySize,
        TokenMemorySegment,
        TokenMemoryBrackets,
        TokenMemoryStackBrackets,
        TokenMemoryBaseRegister,
        TokenMemoryIndexRegister,
        TokenMemoryScale,
        TokenMemoryOperator, //'+', '-' and '*'
        //registers
        TokenGeneralRegister,
        TokenFpuRegister,
        TokenMmxRegister,
        TokenSseRegister
    };

    struct BeaTokenValue
    {
        int size; //value size
        int_t value; //value
    };

    struct BeaSingleToken
    {
        BeaTokenType type; //token type
        QString text; //text to display
        BeaTokenValue value; //jump destination/displacement/immediate
    };

    struct BeaInstructionToken
    {
        QList<BeaSingleToken> tokens; //list of tokens that form the instruction
        unsigned long hash; //complete instruction token checksum
        int x; //x of the first character
    };

    struct BeaTokenColor
    {
        QString color;
        QString backgroundColor;
    };

    static void Init();
    static unsigned long HashInstruction(const DISASM* disasm);
    static void TokenizeInstruction(BeaInstructionToken* instr, const DISASM* disasm);
    static void TokenToRichText(const BeaInstructionToken* instr, QList<RichTextPainter::CustomRichText_t>* richTextList);

private:
    //variables
    static QMap<BeaTokenType, BeaTokenColor> colorNamesMap;
    static QStringList segmentNames;
    static QMap<int, QString> memSizeNames;
    static QMap<int, QMap<ARGUMENTS_TYPE, QString>> registerMap;

    //functions
    static void AddToken(BeaInstructionToken* instr, const BeaTokenType type, const QString text, const BeaTokenValue* value);
    static void Prefix(BeaInstructionToken* instr, const DISASM* disasm);
    static bool IsNopInstruction(QString mnemonic, const DISASM* disasm);
    static void StringInstructionMemory(BeaInstructionToken* instr, int size, QString segment, ARGUMENTS_TYPE reg);
    static void StringInstruction(QString mnemonic, BeaInstructionToken* instr, const DISASM* disasm);
    static void Mnemonic(BeaInstructionToken* instr, const DISASM* disasm);
    static QString PrintValue(const BeaTokenValue* value, bool module);
    static QString RegisterToString(int size, int reg);
    static void Argument(BeaInstructionToken* instr, const DISASM* disasm, const ARGTYPE* arg, bool* hadarg);
    static void AddColorName(BeaTokenType type, QString color, QString backgroundColor);
};

#endif // BEATOKENIZER_H
