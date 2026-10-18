#pragma once

// 이전 include 경로와 명령 선언 매크로의 호환 계층. 제품 값은 productDefaults.hpp에 둔다.
#include "productDefaults.hpp"

#define DECLARE_HC_COMMAND( Name ) \
    Q_INVOKABLE void Name( const QModelIndex& CursorIndex )

#define DECLARE_HC_COMMAND_EX( Name ) \
    Q_INVOKABLE void Name( const QModelIndex& CursorIndex, const QVariant& Parameter )

#define DEFINE_HC_COMMAND( Class, Name ) \
    void Class:: ##Name( const QModelIndex& CursorIndex )

#define DEFINE_HC_COMMAND_EX( Class, Name ) \
    void Class:: ##Name( const QModelIndex& CursorIndex, const QVariant& Parameter )
