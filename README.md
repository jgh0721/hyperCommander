HyperCommander
==============

Total Commander �� ����� ���� ������. 

��ǥ
====

1. Qt + QtitanDataGrid �� �̿��Ͽ� ��Ż Ŀ�Ǵ��� ��� ����( �н��� )
2. Total Commander �� ���� ���ϰ����ڿ� ���� UI �� ���� QtitanDataGrid Row/Cell UI Ŀ���͸�����

���
====

1. ���� ����
1.1 �̸� ���� : �ζ��� �̸�����( F2, ���콺 Ŭ�� )
1.2 ���丮 ���� : Ctrl+K 
1.3 ���� ���� : Del
1.4 ���� �̸� ����ȭ : Ctrl+U 

1. ���� ���
1.1 ������ �����ϰ� ������ ��ư�� ���� Ž���� �޴� ǥ��
1.2 �׸����� �� �������� ������ ��ư�� ���� Ž���� �޴� ǥ��
1.3 ���� ��ħ : F5 
1.4 ���� �뷮 : Everything SDK �� �̿��Ͽ� ���� ũ�� ǥ��
1.5 ���� ���� : ���丮 Ž������ ���� ���丮 Ž�� ����
1.6 ��ũ ǥ�� : ���丮�� LINK(���� ��) ������ �� <LINK> ������ ǥ��

TODO
====

1. ���� ���� ����( ����, �̵� )
2. �ϰ� �̸����� ����
4. ����Ű ���� ȭ�� ����
6. ������ �÷����� ����
7. �ܺ� ������ ����
8. ���� ����ǥ ����
9. Etc...

��Ű����
========

FileSystemT -> CFSLocal		-> CFSNtfs, CFSFat, CFSExFAT

FileSystemT -> CFSRemote	-> CFSSmb, CFSSFTP, CFSFtp, CFSWebDAV

FileSourceT -> CFsLocal		-> CFsNtfs

FileEngine 

	���� �۾��� �����ϴ� ����

* FileSourceT �� �Է����� �޴´�. 

	
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

���� ����ǥ
===========

* ��ȭ���� �۲�
* ��ȭ���� ���
* ��ư ������ ����
* ��ư ����
* ��ư ���

ȯ�漳��
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

