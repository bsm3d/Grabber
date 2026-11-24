# Grabber
# © Benoit Saint-Moulin - 2026
# Version du fichier : 1.2.0
# Fonction du fichier : Test helper that opens a titled, coloured window of a given size to serve as a recording target.

import sys
from PySide6.QtWidgets import QApplication, QWidget
from PySide6.QtGui import QColor
title, color, x, y, w, h = sys.argv[1], sys.argv[2], *map(int, sys.argv[3:7])
a = QApplication([])
wd = QWidget()
wd.setWindowTitle(title)
wd.setStyleSheet('background:%s' % color)
wd.setGeometry(x, y, w, h)
wd.show()
a.exec()
