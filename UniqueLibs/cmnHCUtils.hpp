#pragma once

class QFileIconGatherer
{
    
};

enum TyEnSizeStyle {
    HC_SIZE_STYLE_BYTES,
    HC_SIZE_STYLE_KBYTES,
    HC_SIZE_STYLE_BKMG4,        // ( x.xxx  k/M/G )
    HC_SIZE_STYLE_BKMG3,        // ( x.xx   k/M/G )
    HC_SIZE_STYLE_BKMG2,        // ( x.x    k/M/G )
    HC_SIZE_STYLE_BKMG,         // ( x      k/M/G )
    HC_SIZE_STYLE_BKMGT4,       // ( x.xxx  k/M/G/T )
    HC_SIZE_STYLE_BKMGT3,       // ( x.xx   k/M/G/T )
    HC_SIZE_STYLE_BKMGT2,       // ( x.x    k/M/G/T )
    HC_SIZE_STYLE_BKMGT,        // ( x      k/M/G/T )
    HC_SIZE_STYLE_BKM2,         // ( x.x    k/M )
    HC_SIZE_STYLE_BKM,          // ( x      k/M )
};

QString                                 GetFormattedSizeText( qint64 Size, TyEnSizeStyle Style );
QString                                 GetFormattedAttrText( quint32 Attributes, bool IsUpper );

/*!
 * @brief Total Commnader's Detect String Parser Ported from Double Commander's uDetectStr

    {
        Double Commander
        -------------------------------------------------------------------------
        Detect string parser.

        Copyright (C) 2008  Dmitry Kolomiets (B4rr4cuda@rambler.ru)
        Copyright (C) 2009-2022  Alexander Koblov (alexx2000@mail.ru)

        Based on TMathControl by Vimil Saju
  
        This program is free software; you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 2 of the License, or
        (at your option) any later version.

        This program is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with this program. If not, see <http://www.gnu.org/licenses/>.
    }
 */

class CDetectParser
{
private:
    enum EnMathType { EM_NIL, EM_OP, EM_LEFT_BRACKET, EM_RIGHT_BRACKET, EM_OPERAND };
    enum EnOperatorType { 
        EO_NONE,        // NULL
        EO_EQU,         // =
        EO_NEQ,         // != replaced with #
        EO_LES,         // <
        EO_MOR,         // >
        EO_AND,         // &
        EO_OR,          // |
        EO_NOT,         // NOT
    };

    struct MathChar
    {
        EnMathType      Type;
        QString         Operand;        // Type == EM_OPERAND
        EnOperatorType  Op;             // Type == EM_OP
    };

public:
    void                                SetDetectString( const QString& Detect );
    QString                             GetDetectString() const;

    bool                                TestFile( const QString& FilePath );
    bool                                TestFile( const QFileInfo& FileInfo );

private:
    ////////////////////////////////////////////////////////////////////////////////
    ///

    bool                                isOperator( QChar Ch ) const;
    bool                                isOperand( QChar Ch ) const;
    EnOperatorType                      convertCharToOperator( QChar Ch ) const;
    QString                             getOperand( int Mid, int& Len );
    int                                 getPriority( EnOperatorType Op ) const;         // GetPrecedence
    QString                             booleanToStr( bool is ) const;
    bool                                strToBoolean( const QString& is ) const;

    void                                convertInfixToPostfix();
    void                                preprocessString();
    QString                             calculate( QFile& File, const MathChar& lhs, const MathChar& rhs, const MathChar& Op );
    bool                                readFile( QFile& File );

    QString                             DetectStr_;
    QString                             MathStr_;
    QVector< MathChar >                 VecInput_, VecOutput_, VecStack_;

    bool                                IsReadComplete = false;
    QVector< quint8 >                   VecFileData_;
};