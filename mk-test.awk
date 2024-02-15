#
# キー入力を受け取る側は IME をオフにしないと文字化けしたり改行しなかったりする。
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
	print "start-sleep -m 500";
}
END{
	print "[System.Windows.Forms.SendKeys]::SendWait(\"END{ENTER}\")";
}
