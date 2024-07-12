#include "stdafx.h"
#include "cmnHCUtils.hpp"

#include "cmnTypeDefs.hpp"

QString GetFormattedSizeText( qint64 Size, TyEnSizeStyle Style )
{
    QString Ret;

    switch( Style )
    {
        case HC_SIZE_STYLE_BYTES: {
            Ret = QString::number( Size ) + " bytes";
        } break;
        case HC_SIZE_STYLE_KBYTES: {
            if( Size == 0 )
                Ret = QString::number( Size ) + " kbytes";
            else if( Size > 0 && Size <= 1024 )
                Ret = QString::number( 1 ) + " kbytes";
            else 
                Ret = QString::number( Size / KIBI_BYTE );
        } break;
        case HC_SIZE_STYLE_BKMG:
        case HC_SIZE_STYLE_BKMG2:
        case HC_SIZE_STYLE_BKMG3:
        case HC_SIZE_STYLE_BKMG4:
        case HC_SIZE_STYLE_BKMGT:
        case HC_SIZE_STYLE_BKMGT2:
        case HC_SIZE_STYLE_BKMGT3:
        case HC_SIZE_STYLE_BKMGT4: {
            const int Precision = Style >= HC_SIZE_STYLE_BKMGT4 ? HC_SIZE_STYLE_BKMGT - Style : HC_SIZE_STYLE_BKMG - Style;

            if( Style >= HC_SIZE_STYLE_BKMGT && Style <= HC_SIZE_STYLE_BKMGT4 && 
                Size >= TEBI_BYTE )
            {
                Ret = QString::number( static_cast< double >( Size ) / static_cast< double >( TEBI_BYTE ), 'f', Precision ) + " T";
            }
            else if( Size >= GIBI_BYTE )
            {
                Ret = QString::number( static_cast< double >( Size ) / static_cast< double >( GIBI_BYTE ), 'f', Precision ) + " G";
            }
            else if( Size >= MEBI_BYTE )
            {
                Ret = QString::number( static_cast< double >( Size ) / static_cast< double >( MEBI_BYTE ), 'f', Precision ) + " M";
            }
            else if( Size >= KIBI_BYTE )
            {
                Ret = QString::number( static_cast< double >( Size ) / static_cast< double >( KIBI_BYTE ), 'f', Precision ) + " k";
            }
            else if( Size > 0 && Size < KIBI_BYTE )
            {
                Ret = QString::number( static_cast< double >( Size ) / static_cast< double >( KIBI_BYTE ), 'f', Precision ) + " k";
            }
            else
            {
                Ret = QString::number( 0 ) + " k";
            }
        } break;
        case HC_SIZE_STYLE_BKM:
        case HC_SIZE_STYLE_BKM2: {
            if( Size >= MEBI_BYTE )
            {
                if( Style == HC_SIZE_STYLE_BKM )
                    Ret = QString::number( Size / MEBI_BYTE ) + " M";
                else if( Style == HC_SIZE_STYLE_BKM2 )
                    Ret = QString::number( static_cast< double >( Size ) / static_cast< double >( MEBI_BYTE ), 'f', 1 ) + " M";
            }
            else if( Size >= KIBI_BYTE )
            {
                if( Style == HC_SIZE_STYLE_BKM )
                    Ret = QString::number( Size / KIBI_BYTE ) + " k";
                else if( Style == HC_SIZE_STYLE_BKM2 )
                    Ret = QString::number( static_cast< double >( Size ) / static_cast< double >( KIBI_BYTE ), 'f', 1 ) + " k";
            }
            else if( Size > 0 && Size < KIBI_BYTE )
            {
                if( Style == HC_SIZE_STYLE_BKM )
                    Ret = QString::number( 1 ) + " k";
                else if( Style == HC_SIZE_STYLE_BKM2 )
                    Ret = QString::number( static_cast< double >( Size ) / static_cast< double >( KIBI_BYTE ), 'f', 1 ) + " k";
            }
            else
            {
                Ret = QString::number( 0 ) + " k";
            }

        } break;
    }

    return Ret;
}

QString GetFormattedAttrText( quint32 Attributes, bool IsUpper )
{
    const char Lower = IsUpper ? 0 : 'a' - 'A';

    QString Ret( 4, '-' );

    Ret[0] = QChar::fromLatin1( FlagOn( Attributes, FILE_ATTRIBUTE_READONLY ) ? 'R' + Lower : '-' );
    Ret[1] = QChar::fromLatin1( FlagOn( Attributes, FILE_ATTRIBUTE_ARCHIVE ) ? 'A' + Lower : '-' );
    Ret[2] = QChar::fromLatin1( FlagOn( Attributes, FILE_ATTRIBUTE_HIDDEN ) ? 'H' + Lower : '-' );
    Ret[3] = QChar::fromLatin1( FlagOn( Attributes, FILE_ATTRIBUTE_SYSTEM ) ? 'S' + Lower : '-' );

    return Ret;
}

QVariant ShowMSGBox( QWidget* parent, QMessageBox::Icon Icon, const QString& Content, const QString& Title, QMessageBox::StandardButtons Buttons, bool IsWait /* = true */, Qt::WindowFlags Flags /* = Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint */ )
{
    return ShowMSGBoxWithDetail( parent, Icon, Content, "", Title, Buttons, IsWait, Flags );
}

QVariant ShowMSGBoxWithDetail( QWidget* parent, QMessageBox::Icon Icon, const QString& Content, const QString& Detail,
                               const QString& Title /* = "" */, QMessageBox::StandardButtons Buttons /* = QMessageBox::Ok */, bool IsWait /* = true */, Qt::WindowFlags Flags /* = Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint | Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint */ )
{
    const auto msgBox = new QMessageBox( Icon, Title, Content, Buttons, parent, Flags );

    msgBox->setProperty( "own", QVariant( true ) );
    msgBox->setAttribute( Qt::WA_DeleteOnClose, true );

    // msgBox.setAttribute( Qt::WA_TranslucentBackground, true );
    msgBox->setWindowIcon( QIcon( ":/titleBar/Resources/iMonFTS-Enable.ico" ) );

    if( Detail.isEmpty() == false )
        msgBox->setDetailedText( Detail );

    // msgBox->setStyleSheet( QMSGBOX_BASE_CSS );

    for( const auto btn : msgBox->buttons() )
    {
        btn->setFixedSize( 80, 36 );
        // btn->setStyleSheet( QMSGBOX_BUTTON_CSS );

        btn->setCheckable( true );
    }

    if( Buttons.testFlag( QMessageBox::Ok ) )
        msgBox->button( QMessageBox::Ok )->setText( QObject::tr( "확인" ) );

    if( Buttons.testFlag( QMessageBox::Save ) )
        msgBox->button( QMessageBox::Save )->setText( QObject::tr( "저장" ) );

    if( Buttons.testFlag( QMessageBox::SaveAll ) )
        msgBox->button( QMessageBox::SaveAll )->setText( QObject::tr( "모두 저장" ) );

    if( Buttons.testFlag( QMessageBox::Open ) )
        msgBox->button( QMessageBox::Open )->setText( QObject::tr( "열기" ) );

    if( Buttons.testFlag( QMessageBox::Yes ) )
        msgBox->button( QMessageBox::Yes )->setText( QObject::tr( "예" ) );

    if( Buttons.testFlag( QMessageBox::YesToAll ) )
        msgBox->button( QMessageBox::YesToAll )->setText( QObject::tr( "모두 예" ) );

    if( Buttons.testFlag( QMessageBox::No ) )
        msgBox->button( QMessageBox::No )->setText( QObject::tr( "아니오" ) );

    if( Buttons.testFlag( QMessageBox::NoToAll ) )
        msgBox->button( QMessageBox::NoToAll )->setText( QObject::tr( "모두 아니오" ) );

    if( Buttons.testFlag( QMessageBox::Abort ) )
        msgBox->button( QMessageBox::Abort )->setText( QObject::tr( "중지" ) );

    if( Buttons.testFlag( QMessageBox::Retry ) )
        msgBox->button( QMessageBox::Retry )->setText( QObject::tr( "재시도" ) );

    if( Buttons.testFlag( QMessageBox::Ignore ) )
        msgBox->button( QMessageBox::Ignore )->setText( QObject::tr( "무시" ) );

    if( Buttons.testFlag( QMessageBox::Close ) )
        msgBox->button( QMessageBox::Close )->setText( QObject::tr( "닫기" ) );

    if( Buttons.testFlag( QMessageBox::Cancel ) )
        msgBox->button( QMessageBox::Cancel )->setText( QObject::tr( "취소" ) );

    if( Buttons.testFlag( QMessageBox::Apply ) )
        msgBox->button( QMessageBox::Apply )->setText( QObject::tr( "적용" ) );

    const auto Screen = msgBox->screen();

    if( Screen != nullptr )
    {
        const auto ScreenRect = Screen->availableGeometry();
        const auto MsgRect = msgBox->sizeHint();
        msgBox->move( ScreenRect.width() / 2 - MsgRect.width() / 2,
                      ScreenRect.height() / 2 - MsgRect.height() / 2 );
    }

    if( IsWait == true )
        msgBox->setModal( false );

    if( IsWait == true )
        return msgBox->exec();

    // show 는 비동기이므로 이 시점에서는 사용자가 누른 버튼을 알 수 없다. 
    msgBox->show();
    return QMessageBox::Ok;
}

void SetTitleIcon( QLabel* Widget )
{
    Q_ASSERT( Widget != nullptr );
    if( Widget == nullptr )
        return;

    Widget->setPixmap( QPixmap( ":/HyperCommander/Logo" ) );
    Widget->setScaledContents( true );
}

bool OpenShellContextMenuForObject( const std::wstring& path, int xPos, int yPos, void* parentWindow )
{
    //assert( parentWindow );
    ITEMIDLIST* id = nullptr;
    IShellFolder* ifolder = nullptr;
    IContextMenu* imenu = nullptr;
    std::wstring windowsPath = path;
    bool IsSuccess = false;

    do
    {
        //std::replace( windowsPath.begin(), windowsPath.end(), '/', '\');
        HRESULT result = SHParseDisplayName( windowsPath.c_str(), 0, &id, 0, 0 );
        if( !SUCCEEDED( result ) || !id )
            break;

        LPCITEMIDLIST idChild = 0;
        result = SHBindToParent( id, IID_IShellFolder, ( void** )&ifolder, &idChild );
        if( !SUCCEEDED( result ) || !ifolder )
            break;

        result = ifolder->GetUIObjectOf( ( HWND )parentWindow, 1, &idChild, IID_IContextMenu, 0, reinterpret_cast< void** >( &imenu ) );
        if( !SUCCEEDED( result ) || !ifolder )
            break;

        HMENU hMenu = CreatePopupMenu();
        if( !hMenu )
            break;

        if( SUCCEEDED( imenu->QueryContextMenu( hMenu, 0, 1, 0x7FFF, CMF_NORMAL ) ) )
        {
            UINT Flags = TPM_RETURNCMD ;
            if( GetSystemMetrics( SM_MENUDROPALIGNMENT ) != 0 )
                Flags |= TPM_RIGHTALIGN | TPM_HORNEGANIMATION;
            else
                Flags |= TPM_LEFTALIGN | TPM_HORPOSANIMATION;

            int iCmd = TrackPopupMenuEx( hMenu, Flags, xPos, yPos, ( HWND )parentWindow, NULL );
            if( iCmd > 0 )
            {
                CMINVOKECOMMANDINFOEX info = { 0 };
                info.cbSize = sizeof( info );
                info.fMask = CMIC_MASK_UNICODE ;
                info.hwnd = ( HWND )parentWindow;
                info.lpVerb = MAKEINTRESOURCEA( iCmd - 1 );
                info.lpVerbW = MAKEINTRESOURCEW( iCmd - 1 );
                info.nShow = SW_SHOWNORMAL;
                imenu->InvokeCommand( ( LPCMINVOKECOMMANDINFO )&info );
            }
        }

        DestroyMenu( hMenu );

        IsSuccess = true;

    } while( false );

    if( imenu )
        imenu->Release();

    if( ifolder )
        ifolder->Release();

    if( id )
        CoTaskMemFree( id );

    return IsSuccess;
}

QString ConvertSTRRETTo( STRRET* Str )
{
    if( Str == nullptr )
        return "";

    if( Str->uType == STRRET_CSTR )
        return QString::fromLocal8Bit( Str->cStr );

    if( Str->uType == STRRET_OFFSET )
        ;

    const auto Ret = QString::fromWCharArray( Str->pOleStr );
    CoTaskMemFree( Str->pOleStr );
    return Ret;
}

////////////////////////////////////////////////////////////////////////////////

void CDetectParser::SetDetectString( const QString& Detect )
{
    if( Detect.compare( DetectStr_, Qt::CaseInsensitive ) == 0 )
        return;

    DetectStr_  = Detect;
    MathStr_    = Detect;

    convertInfixToPostfix();
}

QString CDetectParser::GetDetectString() const
{
    return DetectStr_;
}

bool CDetectParser::TestFile( const QString& FilePath )
{
    return TestFile( QFileInfo( FilePath ) );
}

bool CDetectParser::TestFile( const QFileInfo& FileInfo )
{
    if( MathStr_.isEmpty() == true )
        return true;

    if( FileInfo.exists() == false )
        return false;

    IsReadComplete = false;
    VecStack_.clear();

    QFile File( FileInfo.filePath() );
    if( File.open( QIODeviceBase::ReadOnly ) == false )
        return false;

    for( int idx = 0; idx < VecOutput_.size(); ++idx )
    {
        if( VecOutput_[ idx ].Type == EM_OPERAND )
        {
            VecStack_.resize( VecStack_.size() + 1 );
            VecStack_[ VecStack_.size() - 1 ] = VecOutput_[ idx ];
        }
        else if( VecOutput_[ idx ].Type == EM_OP )
        {
            if( VecStack_.size() > 1 )
            {
                auto tmp1 = VecStack_[ VecStack_.size() - 1 ];
                auto tmp2 = VecStack_[ VecStack_.size() - 2 ];
                VecStack_.resize( VecStack_.size() - 2 );
                decltype( tmp2 ) tmp3;
                tmp3.Type = EM_OPERAND;
                tmp3.Operand = calculate( File, tmp2, tmp1, VecOutput_[idx]);
                VecStack_.resize( VecStack_.size() + 1 );
                VecStack_[ VecStack_.size() - 1 ] = tmp3;
            }
        }
    }
    bool IsMatch = VecStack_.isEmpty() == false && strToBoolean( VecStack_[ 0 ].Operand );
    VecStack_.clear();
    return IsMatch;
}

////////////////////////////////////////////////////////////////////////////////
///

bool CDetectParser::isOperator( QChar Ch ) const
{
    static QSet< QChar > SetOp( { '=', '#', '!', '&', '<', '>', '|' });

    return SetOp.contains( Ch );
}

bool CDetectParser::isOperand( QChar Ch ) const
{
    static QSet< QChar > SetOp( { '=', '#', '!', '&', '<', '>', '|', '(', ')', ' ' });
    return !SetOp.contains( Ch );
}

CDetectParser::EnOperatorType CDetectParser::convertCharToOperator( QChar Ch ) const
{
    if( Ch == '<' ) return EO_LES;
    if( Ch == '>' ) return EO_MOR;
    if( Ch == '&' ) return EO_AND;
    if( Ch == '=' ) return EO_EQU;
    if( Ch == '#' ) return EO_NEQ;
    if( Ch == '!' ) return EO_NOT;
    if( Ch == '|' ) return EO_OR;

    return EO_NONE;
}

QString CDetectParser::getOperand( int Mid, int& Len )
{
    QString Ret;

    Len = MathStr_.length();
    if( MathStr_[ Mid ] == '"' )
    {
        Ret = MathStr_[ Mid ];
        for( int idx = Mid + 1; idx < Len; ++idx )
        {
            Ret += MathStr_[ idx ];
            if( MathStr_[ idx ] == '"' )
                break;
        }
    }
    else
    {
        Ret = "";
        for( int idx = Mid; idx < Len; ++idx )
        {
            if( isOperand( MathStr_[ idx ] ) )
                Ret += MathStr_[ idx ];
            else
                break;
        }
    }

    Len = Ret.length();
    return Ret;
}

int CDetectParser::getPriority( EnOperatorType Op ) const
{
    switch( Op )
    {
        case EO_OR: return 0;
        case EO_AND: return 1;
        case EO_EQU: return 2;
        case EO_NEQ: return 2;
        case EO_LES: return 2;
        case EO_MOR: return 2;
        case EO_NOT: return 2;
        default: return -1;
    }

    return -1;
}

QString CDetectParser::booleanToStr( bool is ) const
{
    if( is == true )
        return QLatin1String( "true" );
    
    return QLatin1String( "false" );
}

bool CDetectParser::strToBoolean( const QString& is ) const
{
    if( is.compare( "true", Qt::CaseInsensitive ) == 0 )
        return true;

    if( is.compare( "false", Qt::CaseInsensitive ) == 0 )
        return false;

    return false;
}

void CDetectParser::convertInfixToPostfix()
{
    preprocessString();

    for( int idx = 0; idx < VecInput_.size(); ++idx )
    {
        const auto& Item = VecInput_[ idx ];
        if( Item.Type == EM_OPERAND )
        {
            VecOutput_.resize( VecOutput_.size() + 1 );
            VecOutput_[ VecOutput_.size() - 1 ] = VecInput_[ idx ];
        }
        else if( Item.Type == EM_LEFT_BRACKET )
        {
            VecStack_.resize( VecStack_.size() + 1 );
            VecStack_[ VecStack_.size() - 1 ] = VecInput_[ idx ];
        }
        else if( Item.Type == EM_OP )
        {
            int priority = getPriority( VecInput_[ idx ].Op );
            int j = VecStack_.size() - 1;
            if( j >= 0 )
            {
                while( j >= 0 && getPriority( VecStack_[ j ].Op ) >= priority )
                {
                    VecOutput_.resize( VecOutput_.size() + 1 );
                    VecOutput_[ VecOutput_.size() - 1 ] = VecStack_[ j ];
                    VecStack_.resize( VecStack_.size() - 1 );
                    j -= 1;
                }
                VecStack_.resize( VecStack_.size() + 1 );
                VecStack_[ VecStack_.size() - 1 ] = VecInput_[ idx ];
            }
        }
        else if( Item.Type == EM_RIGHT_BRACKET )
        {
            int j = VecStack_.size() - 1;
            if( j >= 0 )
            {
                while( j >= 0 && VecStack_[ j ].Type != EM_LEFT_BRACKET )
                {
                    VecOutput_.resize( VecOutput_.size() + 1 );
                    VecOutput_[ VecOutput_.size() - 1 ] = VecStack_[ j ];
                    VecStack_.resize( VecStack_.size() - 1 );
                    j -= 1;
                }

                if( j >= 0 )
                    VecStack_.resize( VecStack_.size() - 1 );
            }
        }
    }
}

void CDetectParser::preprocessString()
{
    MathStr_ = MathStr_.replace( "FIND(", "FIND=(", Qt::CaseInsensitive );
    MathStr_ = MathStr_.replace( "!=", "#", Qt::CaseInsensitive );
    MathStr_ = MathStr_.replace( "FINDI(", "FINDI=(", Qt::CaseInsensitive );
    MathStr_ = MathStr_.replace( "MULTIMEDIA", "true", Qt::CaseInsensitive );
    MathStr_ = MathStr_.replace( "FORCE", "BooleanToStr(FForce)", Qt::CaseInsensitive );

    int Idx = 0;
    int NumLen = 1;

    while( NumLen < MathStr_.length() )
    {
        if( MathStr_[ NumLen ] == '!' &&
            MathStr_[ NumLen + 1 ] != '=' )
        {
            Idx = NumLen;
            MathStr_.remove( Idx, 1 );
            MathStr_.insert( Idx, "NOT!" );
            NumLen += 4;
        }
        else
        {
            NumLen += 1;
        }
    }

    Idx = 0;
    NumLen = 0;
    VecInput_.clear();
    VecStack_.clear();
    VecOutput_.clear();
    MathStr_ = '(' + MathStr_ + ')';
    VecInput_.resize( MathStr_.length() );

    const auto q = MathStr_.length();

    while( Idx < MathStr_.length() )
    {
        if( MathStr_[ Idx ] == '(' )
        {
            VecInput_[ Idx ].Type = EM_LEFT_BRACKET;
            Idx++;
        }
        else if( MathStr_[ Idx ] == ')' )
        {
            VecInput_[ Idx ].Type = EM_RIGHT_BRACKET;
            Idx++;
        }
        else if( isOperator( MathStr_[ Idx ] ) )
        {
            VecInput_[ Idx ].Type   = EM_OP;
            VecInput_[ Idx ].Op     = convertCharToOperator( MathStr_[ Idx] );
            Idx++;
        }
        else if( isOperand( MathStr_[ Idx ] ) )
        {
            VecInput_[ Idx ].Type       = EM_OPERAND;
            VecInput_[ Idx ].Operand    = getOperand( Idx, NumLen );
            Idx += NumLen;
        }
        else if( MathStr_[ Idx ] == ' ' )
        {
            Idx++;
        }
    }
}

/* Return the first occurrence of NEEDLE in HAYSTACK.  */
void* memmem( const void* haystack, size_t haystack_len, const void* needle, size_t needle_len )
{
    const char* begin;
    const char* const last_possible = ( const char* )haystack + haystack_len - needle_len;

    if( needle_len == 0 )
        /* The first occurrence of the empty string is deemed to occur at
           the beginning of the string.  */
        return ( void* )haystack;

    /* Sanity check, otherwise the loop might search through the whole
       memory.  */
    if( haystack_len < needle_len )
        return NULL;

    for( begin = ( const char* )haystack; begin <= last_possible; ++begin )
        if( begin[ 0 ] == ( ( const char* )needle )[ 0 ] &&
        !memcmp( ( const void* )&begin[ 1 ],
                 ( const void* )( ( const char* )needle + 1 ),
                 needle_len - 1 ) )
            return ( void* )begin;
    
    return NULL;
}

/* Return the first occurrence of NEEDLE in HAYSTACK.  */
void* memimem( const void* haystack, size_t haystack_len, const void* needle, size_t needle_len )
{
    const char* begin;
    const char* const last_possible = ( const char* )haystack + haystack_len - needle_len;

    if( needle_len == 0 )
        /* The first occurrence of the empty string is deemed to occur at
           the beginning of the string.  */
        return ( void* )haystack;

    /* Sanity check, otherwise the loop might search through the whole
       memory.  */
    if( haystack_len < needle_len )
        return NULL;

    for( begin = ( const char* )haystack; begin <= last_possible; ++begin )
        if( begin[ 0 ] == ( ( const char* )needle )[ 0 ] &&
        !_memicmp( ( const void* )&begin[ 1 ],
                   ( const void* )( ( const char* )needle + 1 ),
                   needle_len - 1 ) )
            return ( void* )begin;

    return NULL;
}

QString CDetectParser::calculate( QFile& File, const MathChar& lhs, const MathChar& rhs, const MathChar& Op )
{
    //function TParserControl.Calculate(aFile: TFile; operand1, operand2, Aoperator: TMathChar): String;
    //var
    //  ASize: Int64;
    //  AValue: Boolean;
    //  AChar, Index: Integer;
    //  tmp, data1, data2: String;

    // operand1 => lhs
    // operand2 => rhs
    QString Ret = "false";

    QString data1 = lhs.Operand.toUpper();

    // NOT
    if( ( lhs.Operand.compare( "NOT", Qt::CaseInsensitive ) == 0 ) &&
        ( ( rhs.Operand.compare( "true", Qt::CaseInsensitive ) == 0 || rhs.Operand.compare( "false", Qt::CaseInsensitive ) == 0 ) ) )
    {
        Ret = booleanToStr( !strToBoolean( rhs.Operand ) );
    }

    // & |
    if( ( lhs.Operand.compare( "true", Qt::CaseInsensitive ) == 0 || lhs.Operand.compare( "false", Qt::CaseInsensitive ) == 0 ) &&
        ( rhs.Operand.compare( "true", Qt::CaseInsensitive ) == 0 || rhs.Operand.compare( "false", Qt::CaseInsensitive ) == 0 ) )
    {
        if( Op.Op == EO_AND )
            Ret = booleanToStr( strToBoolean( lhs.Operand ) && strToBoolean( rhs.Operand ) );
        else if( Op.Op == EO_OR )
            Ret = booleanToStr( strToBoolean( lhs.Operand ) || strToBoolean( rhs.Operand ) );
    }

    // [X]= [X]!=
    if( data1.startsWith( '[' ) && data1.endsWith( ']' ) )
    {
        if( IsReadComplete == true )
        {
            if( VecFileData_.isEmpty() == true )
                return Ret;
        }
        else
        {
            if( readFile( File ) == false )
                return Ret;
        }

        const auto Index = data1.mid( 1, data1.size() - 2 ).toInt();

        if( Index >= 0 && Index < VecFileData_.size() )
        {
            char Ch = '\0';

            if( rhs.Operand.size() > 2 && rhs.Operand.startsWith( '"' ) == true && rhs.Operand.endsWith( '"' ) == true )
            {
                Ch = rhs.Operand[ 1 ].toLatin1();
            }
            else
            {
                bool isConvert = false;
                Ch = static_cast< char >( rhs.Operand.toInt( &isConvert) );
                if( isConvert == false )
                    return {};
            }

            Ret = booleanToStr( VecFileData_[ Index ] == Ch );
        }
    }

    // FIND FINDI
    if( data1 == "FIND" || data1 == "FINDI" )
    {
        if( IsReadComplete == true )
        {
            if( VecFileData_.isEmpty() == true )
                return Ret;
        }
        else
        {
            if( readFile( File ) == false )
                return Ret;
        }

        if( rhs.Operand.size() > 2 && rhs.Operand.startsWith( '"' ) == true && rhs.Operand.endsWith( '"' ) == true )
        {
            bool IsCaseSensitive = data1 == "FIND";
            const auto tgt = rhs.Operand.mid( 1, rhs.Operand.size() - 2 ).toLatin1();

            if( IsCaseSensitive == true )
                Ret = booleanToStr( memmem( VecFileData_.data(), VecFileData_.size(), tgt.data(), tgt.size() ) != nullptr );
            else
                Ret = booleanToStr( memimem( VecFileData_.data(), VecFileData_.size(), tgt.data(), tgt.size() ) != nullptr );
        }
    }

    // EXT= EXT!=
    if( data1 == "EXT" )
    {
        const auto Ext = '"' + QFileInfo( File ).suffix().toUpper() + '"';
        if( Op.Op == EO_EQU )
            Ret = booleanToStr( QRegularExpression::fromWildcard( rhs.Operand ).match( Ext ).hasMatch() );
        if( Op.Op == EO_NEQ )
            Ret = booleanToStr( !QRegularExpression::fromWildcard( rhs.Operand ).match( Ext ).hasMatch() );
    }

    // SIZE > < = !=
    if( data1 == "SIZE" )
    {
        // QFile( 향후 가상의 File 구조체로 대체 )이 파일 크기를 지원할 때만 들어온다.
        bool isConvert = false;
        const auto Size = rhs.Operand.toLongLong( &isConvert );
        if( isConvert == true )
        {
            if( Op.Op == EO_EQU )
                Ret = booleanToStr( File.size() == Size );
            if( Op.Op == EO_NEQ )
                Ret = booleanToStr( File.size() != Size );
            if( Op.Op == EO_LES )
                Ret = booleanToStr( File.size() < Size );
            if( Op.Op == EO_MOR )
                Ret = booleanToStr( File.size() > Size );
        }
    }

    return Ret;
}

bool CDetectParser::readFile( QFile& File )
{
    bool IsSuccess = false;
    IsReadComplete = true;
    VecFileData_.resize( 8192 );

    const auto data = File.read( 8192 );
    const auto ewq = File.errorString();

    if( data.size() > 0 )
    {
        VecFileData_.resize( data.size() );
        memcpy( VecFileData_.data(), data.data(), data.size() );
        IsSuccess = true;
    }
    else
    {
        VecFileData_.clear();
    }

    return IsSuccess;
}
