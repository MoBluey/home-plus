/* Home+ native: focus sounds, synthesised at start-up (no Apple audio): a soft
 * tick when focus moves and a click on select, kept quiet. The audio device is
 * opened on the first sound and closed again after a few quiet seconds, so an
 * idle Home+ costs no CPU and other apps get the audio to themselves. */
#ifndef HOMEPLUS_SOUND_H
#define HOMEPLUS_SOUND_H

enum { SND_TICK, SND_SELECT, SND_COUNT };

void snd_init(void);
void snd_enable(int on);
void snd_play(int which);
/* Call regularly: closes the device once it has been quiet for a while. */
void snd_idle(void);
/* Another app is in front: release the device now. */
void snd_suspend(void);
/* 1 while the audio device is open (the loop wakes to close it). */
int snd_active(void);

#endif
