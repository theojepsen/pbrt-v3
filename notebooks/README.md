# Dependencies

Install pip packages:

    sudo pip3 install jupyter tqdm pandas ipython ipywidgets graphviz matplotlib ipympl statsmodels sklearn

Enable [widgets exetension](https://ipywidgets.readthedocs.io/en/stable/user_install.html):

    ~/.local/bin/jupyter nbextension enable --py widgetsnbextension

# Running

Start the notebook with passwords/tokens disabled (*insecure!*):

    ~/.local/bin/jupyter notebook --ip=0.0.0.0  --NotebookApp.token='' --NotebookApp.password=''
