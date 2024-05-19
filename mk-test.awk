#
# 使い方
# (1) i.txt を UTF8,LF にする
# (2) set LANG=ja_JP.UTF8
# (3) gawk -f mk-test.awk i.txt > test.ps1
# (4) test.ps1 を UTF8 with BOM, LF にする
# (5) 入力欄にフォーカスしてから ps test.ps1
#
# キー入力を受け取る側は IME をオフにしないと文字化けしたり改行しなかったりする。
# CRLF->LF, ” の変換
#
BEGIN{
	print "Add-Type -AssemblyName System.Windows.Forms";
	print "Start-Sleep -m 500"
	print "[System.Windows.Forms.SendKeys]::SendWait(\"%{TAB}\")";
	print "Start-Sleep -m 500"
}
{
	gsub(/\(/,"{(}",$1);
	gsub(/\)/,"{)}",$1);
	gsub(/\^/,"{^}",$1);
	gsub(/\+/,"{+}",$1);
	printf("[System.Windows.Forms.SendKeys]::SendWait(\"%d:%s{ENTER}\")\n",NR,$1);
	print "start-sleep -m 5000";
}
END{
	print "[System.Windows.Forms.SendKeys]::SendWait(\"END{ENTER}\")";
}
