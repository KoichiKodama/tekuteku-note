#
# 使い方
# (1) i.txt を UTF8,LF にする
# (2) set LANG=ja_JP.UTF8
# (3) gawk -f mk-test.awk i.txt > test.ps1
# (4) test.ps1 を UTF8 with BOM, LF にする
# (5) 入力欄にフォーカスしてから ps test.ps1 <- IME 無効にしておく事 !
#
# キー入力を受け取る側は IME をオフにしないと文字化けしたり改行しなかったりする。
# CRLF->LF, ” の変換
#
BEGIN{
	print "add-type -assemblyname System.Windows.Forms";
	print "start-sleep -m 500"
	print "[System.Windows.Forms.SendKeys]::SendWait(\"%{TAB}\")";
	print "start-sleep -m 500"
}
{
#	gsub(/\(/,"{(}",$1);
#	gsub(/\)/,"{)}",$1);
#	gsub(/\^/,"{^}",$1);
#	gsub(/\+/,"{+}",$1);
#	printf("[System.Windows.Forms.SendKeys]::SendWait(\"%d:%s{ENTER}\")\n",NR,$1);
	printf("[System.Windows.Forms.SendKeys]::SendWait(\"%d：\")\n",NR);
	print "start-sleep -m 50";
	for (i=1;i<=length($1);i++) {
		c = substr($1,i,1);
		gsub(/\(/,"{(}",c);
		gsub(/\)/,"{)}",c);
		gsub(/\^/,"{^}",c);
		gsub(/\+/,"{+}",c);
		printf("[System.Windows.Forms.SendKeys]::SendWait(\"%s\")\n",c);
		print "start-sleep -m 10";
	}
	printf("[System.Windows.Forms.SendKeys]::SendWait(\"{ENTER}\")\n");
	print "start-sleep -m 500";
}
END{
	print "[System.Windows.Forms.SendKeys]::SendWait(\"END{ENTER}\")";
}
