# VibeOS: /usr/share/vibeos first in XDG_DATA_DIRS, where vibeos-hide-replaced-apps hides GNOME's apps that a VibeOS
# app replaces (Files and Nautilus, Terminal and Console...) with copies of their entries made NoDisplay. gnome-session
# starts through a login shell, which reads this after gnome-session_gnomerc.sh (/usr/share/gnome, or the default
# list when there was none)
case ":${XDG_DATA_DIRS:-}:" in
*:/usr/share/vibeos:*) ;;
*)
	XDG_DATA_DIRS=/usr/share/vibeos:${XDG_DATA_DIRS:-/usr/local/share/:/usr/share/}
	export XDG_DATA_DIRS
	;;
esac
