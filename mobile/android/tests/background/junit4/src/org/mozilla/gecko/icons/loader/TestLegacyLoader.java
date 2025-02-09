/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

package org.mozilla.gecko.icons.loader;

import android.graphics.Bitmap;

import org.junit.Assert;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mozilla.gecko.background.testhelpers.TestRunner;
import org.mozilla.gecko.icons.IconDescriptor;
import org.mozilla.gecko.icons.IconRequest;
import org.mozilla.gecko.icons.IconResponse;
import org.mozilla.gecko.icons.Icons;
import org.robolectric.RuntimeEnvironment;

import java.util.Iterator;

import static org.mockito.Mockito.doReturn;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.never;
import static org.mockito.Mockito.spy;
import static org.mockito.Mockito.verify;

@RunWith(TestRunner.class)
public class TestLegacyLoader {
    private static final String TEST_PAGE_URL = "http://www.mozilla.org";
    private static final String TEST_ICON_URL = "https://example.org/favicon.ico";
    private static final String TEST_ICON_URL_2 = "https://example.com/page/favicon.ico";
    private static final String TEST_ICON_URL_3 = "https://example.net/icon/favicon.ico";

    @Test
    public void testDatabaseIsQueriesForNormalRequestsWithNetworkSkipped() {
        final IconRequest request = Icons.with(RuntimeEnvironment.application)
                .pageUrl(TEST_PAGE_URL)
                .icon(IconDescriptor.createGenericIcon(TEST_ICON_URL))
                .skipNetwork()
                .build();

        final LegacyLoader loader = spy(new LegacyLoader());
        final IconResponse response = loader.load(request);

        verify(loader).loadBitmapFromDatabase(request);

        Assert.assertNull(response);
    }

    @Test
    public void testNothingIsLoadedIfNetworkIsNotSkipped() {
        final IconRequest request = Icons.with(RuntimeEnvironment.application)
                .pageUrl(TEST_PAGE_URL)
                .icon(IconDescriptor.createGenericIcon(TEST_ICON_URL))
                .build();

        final LegacyLoader loader = spy(new LegacyLoader());
        final IconResponse response = loader.load(request);

        verify(loader, never()).loadBitmapFromDatabase(request);

        Assert.assertNull(response);
    }

    @Test
    public void testNothingIsLoadedIfDiskSHouldBeSkipped() {
        final IconRequest request = Icons.with(RuntimeEnvironment.application)
                .pageUrl(TEST_PAGE_URL)
                .icon(IconDescriptor.createGenericIcon(TEST_ICON_URL))
                .skipDisk()
                .build();

        final LegacyLoader loader = spy(new LegacyLoader());
        final IconResponse response = loader.load(request);

        verify(loader, never()).loadBitmapFromDatabase(request);

        Assert.assertNull(response);
    }

    @Test
    public void testLoadedBitmapIsReturnedAsResponse() {
        final IconRequest request = Icons.with(RuntimeEnvironment.application)
                .pageUrl(TEST_PAGE_URL)
                .icon(IconDescriptor.createGenericIcon(TEST_ICON_URL))
                .skipNetwork()
                .build();

        final Bitmap bitmap = mock(Bitmap.class);

        final LegacyLoader loader = spy(new LegacyLoader());
        doReturn(bitmap).when(loader).loadBitmapFromDatabase(request);

        final IconResponse response = loader.load(request);

        Assert.assertNotNull(response);
        Assert.assertEquals(bitmap, response.getBitmap());
    }

    @Test
    public void testLoaderOnlyLoadsIfThereIsOneIconLeft() {
        final IconRequest request = Icons.with(RuntimeEnvironment.application)
                .pageUrl(TEST_PAGE_URL)
                .icon(IconDescriptor.createGenericIcon(TEST_ICON_URL))
                .icon(IconDescriptor.createGenericIcon(TEST_ICON_URL_2))
                .icon(IconDescriptor.createGenericIcon(TEST_ICON_URL_3))
                .skipNetwork()
                .build();

        final LegacyLoader loader = spy(new LegacyLoader());
        doReturn(mock(Bitmap.class)).when(loader).loadBitmapFromDatabase(request);

        // First load doesn't load an icon.
        Assert.assertNull(loader.load(request));

        // Second load doesn't load an icon.
        removeFirstIcon(request);
        Assert.assertNull(loader.load(request));

        // Now only one icon is left and a response will be returned.
        removeFirstIcon(request);
        Assert.assertNotNull(loader.load(request));
    }

    private void removeFirstIcon(IconRequest request) {
        final Iterator<IconDescriptor> iterator = request.getIconIterator();
        if (iterator.hasNext()) {
            iterator.next();
            iterator.remove();
        }
    }
}
