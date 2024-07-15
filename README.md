HyperCommander
==============

Total Commander 를 모방한 파일 관리자. 

목표
====

1. Qt + QtitanDataGrid 를 이용하여 토탈 커맨더의 기능 구현( 학습용 )
2. Total Commander 와 같은 파일관리자와 같은 UI 를 위해 QtitanDataGrid Row/Cell UI 커스터마이즈

기능
====

1. 파일 동작
1.1 이름 변경 : 인라인 이름변경( F2, 마우스 클릭 )
1.2 디렉토리 생성 : Ctrl+K 
1.3 파일 삭제 : Del
1.4 파일 이름 정규화 : Ctrl+U 

1. 파일 목록
1.1 파일을 선택하고 오른쪽 버튼을 눌러 탐색기 메뉴 표시
1.2 그리드의 빈 공간에서 오른쪽 버튼을 눌러 탐색기 메뉴 표시
1.3 새로 고침 : F5 
1.4 폴더 용량 : Everything SDK 를 이용하여 폴더 크기 표시
1.5 정션 지원 : 디렉토리 탐색에서 정션 디렉토리 탐색 지원
1.6 링크 표시 : 디렉토리가 LINK(정션 등) 형태일 때 <LINK> 등으로 표시

TODO
====

1. 파일 동작 구현( 복사, 이동 )
2. 일괄 이름변경 구현
4. 단축키 설정 화면 구현
6. 리스터 플러그인 관리
7. 외부 편집기 관리
8. 색상 구성표 적용
9. Etc...

아키텍쳐
========

FileSystemT -> CFSLocal		-> CFSNtfs, CFSFat, CFSExFAT

FileSystemT -> CFSRemote	-> CFSSmb, CFSSFTP, CFSFtp, CFSWebDAV

FileSourceT -> CFsLocal		-> CFsNtfs

FileEngine 

	파일 작업을 수행하는 엔진

* FileSourceT 를 입력으로 받는다. 

	
QAbstractItemModel -> FsModelT 

FsModelT
	GetFS
	SetColorScheme

	MainUI
		CmpPanel
			Header
				VolumeList
				VolumeInfo
			Content( StackedWidget )
				Grid
			Footer
				Directory Info

	CmpView
		GridView
		WlxView
		QuickView

색상 구성표
===========

* 대화상자 글꼴
* 대화상자 배경
* 버튼 프레임 색상
* 버튼 전경
* 버튼 배경

환경설정
========

	[Configuration]

	[Favorites]
	Count=
	MenuN=
	CmdN=
	PathN=
	DrivesOn=0|1|2
		0 = None, 1 = Top, 2 = Bottom

	[Viewer]

	[ListerPlugins]
	Count=N
	N_Path=XXX
	N_Detect=XXX
	N_Support64Bit=True|False

	[PackerPlugins]
	Count=N
	N_Path=XXX
	N_Exts=XXX|XXX
	N_Caps=
	N_Support64Bit=True|False

